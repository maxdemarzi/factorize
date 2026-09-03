#include "factorize/table_function.hpp"

#include "../core/plan.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>
#include <map>

namespace duckdb {

namespace {

using factorize::ColumnStats;
using factorize::Predicate;
using factorize::QueryGraph;
using factorize::RelationSource;

//! One relation: a physical table, the alias it is referenced by, and the
//! columns any predicate touches.
struct BoundRelation {
	string table;
	string alias;
	//! Physical column indexes, in the order the f-representation sees them.
	vector<idx_t> columns;
	//! Column name for each entry of `columns`, for error messages.
	vector<string> column_names;
	//! Storage width for each entry of `columns`, parallel to it.
	vector<factorize::ValueType> column_types;

	int LocalIndex(idx_t physical) const {
		for (idx_t i = 0; i < columns.size(); i++) {
			if (columns[i] == physical) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

struct FactorizedCountBindData : public TableFunctionData {
	vector<BoundRelation> relations;
	QueryGraph graph;
};

//! Splits on whitespace, so "yago2 a" and "yago2 AS a" both parse.
vector<string> SplitWords(const string &text) {
	vector<string> words;
	string current;
	for (char c : text) {
		if (StringUtil::CharacterIsSpace(c)) {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		words.push_back(current);
	}
	return words;
}

string Trim(const string &text) {
	idx_t start = 0;
	idx_t end = text.size();
	while (start < end && StringUtil::CharacterIsSpace(text[start])) {
		start++;
	}
	while (end > start && StringUtil::CharacterIsSpace(text[end - 1])) {
		end--;
	}
	return text.substr(start, end - start);
}

//! An f-representation packs keys into fixed-width slots, so a join key has to
//! be an integer of known width. Rejecting here, by name, beats failing later
//! inside the core with no idea which column was at fault.
//!
//! Returns the storage width the core must use for this column. This is not
//! simply "32-bit types get INT32, 64-bit types get INT64": factorize::INT32
//! is a *signed* 32-bit slot, and UINTEGER's range (0..4294967295) does not
//! fit in one -- a value above INT32_MAX would be silently truncated exactly
//! like the bug this function's caller exists to prevent. UINTEGER needs
//! INT64 storage despite being nominally 32 bits.
factorize::ValueType RequireIntegerKey(const LogicalType &type, const string &alias, const string &column) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
		return factorize::ValueType::INT32;
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
		return factorize::ValueType::INT64;
	default:
		throw BinderException("factorized_count: join key %s.%s is %s; only integer keys are supported", alias, column,
		                      type.ToString());
	}
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<string> &names) {
	auto result = make_uniq<FactorizedCountBindData>();

	if (input.inputs.size() != 2) {
		throw BinderException("factorized_count(tables, joins) takes two lists");
	}
	auto tables = ListValue::GetChildren(input.inputs[0]);
	auto joins = ListValue::GetChildren(input.inputs[1]);
	if (tables.size() < 2) {
		throw BinderException("factorized_count: needs at least two relations, got %llu",
		                      static_cast<uint64_t>(tables.size()));
	}

	// Resolve each relation against the catalog, keeping the alias it will be
	// referenced by. Several relations may name the same table -- that is a
	// self-join, and the alias is the only thing distinguishing them.
	std::map<string, idx_t> relation_of_alias;
	vector<reference<TableCatalogEntry>> entries;
	for (auto &entry : tables) {
		const auto words = SplitWords(StringValue::Get(entry));
		if (words.empty()) {
			throw BinderException("factorized_count: empty table specification");
		}
		BoundRelation relation;
		relation.table = words[0];
		relation.alias = words.size() > 1 && !StringUtil::CIEquals(words[1], "as")
		                     ? words[1]
		                     : (words.size() > 2 ? words[2] : words[0]);
		auto &catalog_entry =
		    Catalog::GetEntry<TableCatalogEntry>(context, INVALID_CATALOG, INVALID_SCHEMA, relation.table);
		if (relation_of_alias.count(relation.alias)) {
			throw BinderException("factorized_count: duplicate relation name '%s'; use an alias", relation.alias);
		}
		relation_of_alias[relation.alias] = result->relations.size();
		entries.emplace_back(catalog_entry);
		result->relations.push_back(std::move(relation));
	}

	// Parse predicates, recording which columns each relation actually needs.
	// Columns no predicate mentions are not read: they cannot change a count,
	// and carrying them would widen every record for nothing.
	struct RawEdge {
		idx_t left_relation, right_relation;
		idx_t left_column, right_column;
	};
	vector<RawEdge> edges;
	for (auto &entry : joins) {
		const auto text = StringValue::Get(entry);
		const auto equals = text.find('=');
		if (equals == string::npos) {
			throw BinderException("factorized_count: '%s' is not an equality", text);
		}
		RawEdge edge {};
		for (int side = 0; side < 2; side++) {
			const auto operand = Trim(side == 0 ? text.substr(0, equals) : text.substr(equals + 1));
			const auto dot = operand.rfind('.');
			if (dot == string::npos) {
				throw BinderException("factorized_count: '%s' must be <relation>.<column>", operand);
			}
			const auto alias = operand.substr(0, dot);
			const auto column = operand.substr(dot + 1);
			auto found = relation_of_alias.find(alias);
			if (found == relation_of_alias.end()) {
				throw BinderException("factorized_count: '%s' does not name a listed relation", alias);
			}
			const idx_t relation = found->second;
			auto &table_entry = entries[relation].get();
			if (!table_entry.ColumnExists(column)) {
				throw BinderException("factorized_count: %s has no column '%s'", alias, column);
			}
			auto &definition = table_entry.GetColumn(column);
			const auto value_type = RequireIntegerKey(definition.Type(), alias, column);
			const auto physical = static_cast<idx_t>(definition.Physical().index);

			auto &bound = result->relations[relation];
			if (bound.LocalIndex(physical) < 0) {
				bound.columns.push_back(physical);
				bound.column_names.push_back(column);
				bound.column_types.push_back(value_type);
			}
			(side == 0 ? edge.left_relation : edge.right_relation) = relation;
			(side == 0 ? edge.left_column : edge.right_column) = physical;
		}
		edges.push_back(edge);
	}

	for (idx_t i = 0; i < result->relations.size(); i++) {
		if (result->relations[i].columns.empty()) {
			throw BinderException("factorized_count: relation '%s' has no join predicate", result->relations[i].alias);
		}
		result->graph.column_counts.push_back(result->relations[i].columns.size());
		result->graph.column_types.push_back(result->relations[i].column_types);
	}
	for (auto &edge : edges) {
		Predicate predicate;
		predicate.left_relation = edge.left_relation;
		predicate.left_column = result->relations[edge.left_relation].LocalIndex(edge.left_column);
		predicate.right_relation = edge.right_relation;
		predicate.right_column = result->relations[edge.right_relation].LocalIndex(edge.right_column);
		result->graph.predicates.push_back(predicate);
	}

	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("count");
	return std::move(result);
}

//! Reads relations out of DuckDB storage on demand.
//!
//! Materialises one relation at a time. The queries this engine exists for have
//! small inputs and enormous results, so holding every base table at once would
//! be the wrong trade even though it would be simpler.
class StorageSource : public RelationSource {
public:
	StorageSource(ClientContext &context, const FactorizedCountBindData &bind_data)
	    : context(context), bind_data(bind_data) {
	}

	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override {
		if (loaded_relation == relation && !held.empty()) {
			return held;
		}
		Load(relation);
		loaded_relation = relation;
		return held;
	}

	ColumnStats Stats(size_t relation, size_t column) override {
		const auto &columns = Columns(relation);
		ColumnStats stats;
		stats.rows = static_cast<double>(columns[column].size());
		// Exact statistics, computed from the data just read. DuckDB's catalog
		// carries approximate distinct counts and no MCV list at all, so Phase 3
		// has to sample instead -- see DECISIONS D13. Doing it exactly here keeps
		// Phase 2 about data movement rather than estimation quality.
		auto sorted = columns[column];
		std::sort(sorted.begin(), sorted.end());
		double distinct = 0;
		std::vector<std::pair<int64_t, double>> frequencies;
		for (size_t i = 0; i < sorted.size();) {
			size_t j = i;
			while (j < sorted.size() && sorted[j] == sorted[i]) {
				j++;
			}
			distinct += 1;
			frequencies.emplace_back(sorted[i], static_cast<double>(j - i));
			i = j;
		}
		std::sort(frequencies.begin(), frequencies.end(),
		          [](const std::pair<int64_t, double> &a, const std::pair<int64_t, double> &b) {
			          return a.second > b.second;
		          });
		if (frequencies.size() > MCV_ENTRIES) {
			frequencies.resize(MCV_ENTRIES);
		}
		stats.distinct = std::max(1.0, distinct);
		stats.mcv = std::move(frequencies);
		return stats;
	}

private:
	//! Matches the harness (DECISIONS D13): measured across five datasets, 128
	//! entries is where hetio's error settles and more buys almost nothing.
	static constexpr size_t MCV_ENTRIES = 128;

	void Load(size_t relation) {
		const auto &bound = bind_data.relations[relation];
		auto &entry = Catalog::GetEntry<TableCatalogEntry>(context, INVALID_CATALOG, INVALID_SCHEMA, bound.table);
		auto &storage = entry.GetStorage();
		auto &transaction = DuckTransaction::Get(context, entry.catalog);

		vector<StorageIndex> column_ids;
		vector<LogicalType> types;
		for (auto physical : bound.columns) {
			column_ids.emplace_back(physical);
			types.push_back(entry.GetColumns().GetColumn(PhysicalIndex(physical)).Type());
		}

		TableScanState state;
		storage.InitializeScan(context, transaction, state, column_ids);

		held.assign(bound.columns.size(), {});
		DataChunk chunk;
		chunk.Initialize(Allocator::Get(context), types);
		while (true) {
			chunk.Reset();
			storage.Scan(transaction, chunk, state);
			if (chunk.size() == 0) {
				break;
			}
			// A relation's columns must stay row-aligned: held[0][k] and
			// held[1][k] have to describe the same source row, because MakeScan
			// zips them back together by shared index with no row id attached.
			// Deciding a row's fate (kept or dropped) requires looking at every
			// column *before* pushing any of them -- checking and pushing one
			// column at a time, independently, drops rows from whichever
			// columns happen to hold a NULL and desynchronizes every row after
			// the first such NULL for a relation with more than one join
			// column. This was a live bug (FINDINGS): filter once per row,
			// across all columns, or not at all.
			std::vector<UnifiedVectorFormat> formats(bound.columns.size());
			for (idx_t c = 0; c < bound.columns.size(); c++) {
				chunk.data[c].ToUnifiedFormat(chunk.size(), formats[c]);
			}
			for (idx_t row = 0; row < chunk.size(); row++) {
				bool all_valid = true;
				for (idx_t c = 0; c < bound.columns.size() && all_valid; c++) {
					all_valid = formats[c].validity.RowIsValid(formats[c].sel->get_index(row));
				}
				if (!all_valid) {
					// A NULL in any of this relation's join columns can never
					// equal anything, so the row can never contribute to an
					// inner join's count.
					continue;
				}
				for (idx_t c = 0; c < bound.columns.size(); c++) {
					const auto index = formats[c].sel->get_index(row);
					held[c].push_back(ValueToInt64(chunk.data[c].GetType(), formats[c], index));
				}
			}
		}
	}

	static int64_t ValueToInt64(const LogicalType &type, const UnifiedVectorFormat &format, idx_t index) {
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
			return UnifiedVectorFormat::GetData<int8_t>(format)[index];
		case LogicalTypeId::SMALLINT:
			return UnifiedVectorFormat::GetData<int16_t>(format)[index];
		case LogicalTypeId::INTEGER:
			return UnifiedVectorFormat::GetData<int32_t>(format)[index];
		case LogicalTypeId::BIGINT:
			return UnifiedVectorFormat::GetData<int64_t>(format)[index];
		case LogicalTypeId::UTINYINT:
			return UnifiedVectorFormat::GetData<uint8_t>(format)[index];
		case LogicalTypeId::USMALLINT:
			return UnifiedVectorFormat::GetData<uint16_t>(format)[index];
		case LogicalTypeId::UINTEGER:
			return UnifiedVectorFormat::GetData<uint32_t>(format)[index];
		case LogicalTypeId::UBIGINT:
			return static_cast<int64_t>(UnifiedVectorFormat::GetData<uint64_t>(format)[index]);
		default:
			throw InternalException("factorized_count: unsupported key type %s", type.ToString());
		}
	}

	ClientContext &context;
	const FactorizedCountBindData &bind_data;
	std::vector<std::vector<int64_t>> held;
	size_t loaded_relation = static_cast<size_t>(-1);
};

struct FactorizedCountGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
};

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<FactorizedCountGlobalState>();
}

void Execute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FactorizedCountGlobalState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	state.emitted = true;

	auto &bind_data = data.bind_data->Cast<FactorizedCountBindData>();

	// Bound to DuckDB's own memory_limit. Without this the engine allocates
	// until the kernel kills the process -- which is exactly what happened on
	// the CE corpus, taking the whole DuckDB session with it. An f-representation
	// that silently OOMs is worse than one that is slow, so the core's allocator
	// is capped and a query that would exceed the cap fails cleanly instead.
	//
	// Half the limit, not all of it: the base-table columns being scanned are
	// held outside the arena and are not counted by it.
	const auto &config = DBConfig::GetConfig(context);
	const idx_t available = config.options.maximum_memory == DConstants::INVALID_INDEX
	                            ? static_cast<idx_t>(4) * 1024 * 1024 * 1024
	                            : config.options.maximum_memory;
	factorize::SetGlobalMemoryLimit(static_cast<size_t>(available / 2));

	const auto plan = factorize::BuildPlan(bind_data.graph);
	if (!plan.complete) {
		throw InvalidInputException("factorized_count: %s", plan.reason);
	}

	StorageSource source(context, bind_data);
	// Bottom-insert is the mode that carries the benefit (FINDINGS F4:
	// bottom-inserts alone are worth 1.9x, top-inserts 0.98x). Phase 3 chooses
	// per join; Phase 2 is about data movement, so it takes the better default.
	const auto result = factorize::ExecuteCount(bind_data.graph, plan, source, factorize::JoinMode::BOTTOM_INSERT);
	if (!result.ok) {
		throw InvalidInputException("factorized_count: %s", result.error);
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BIGINT(result.count));
}

} // namespace

void RegisterFactorizedCount(ExtensionLoader &loader) {
	TableFunction function("factorized_count",
	                       {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)}, Execute,
	                       Bind, InitGlobal);
	loader.RegisterFunction(function);
}

} // namespace duckdb
