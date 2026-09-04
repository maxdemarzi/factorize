#include "factorize/table_function.hpp"

#include "factorize/storage_source.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"

#include <map>

namespace duckdb {

namespace {

using factorize::Predicate;
using factorize::QueryGraph;

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

//! Named the relation explicitly, the caller gets told why its key will not do
//! -- failing later inside the core with no idea which column was at fault is
//! the alternative. The optimizer rule wants the opposite of this and declines
//! silently, which is why the type mapping itself lives in TryIntegerKeyType.
factorize::ValueType RequireIntegerKey(const LogicalType &type, const string &alias, const string &column) {
	factorize::ValueType value_type;
	if (!TryIntegerKeyType(type, value_type)) {
		throw BinderException("factorized_count: join key %s.%s is %s; only integer keys are supported", alias, column,
		                      type.ToString());
	}
	return value_type;
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
		relation.alias = words.size() > 1 && !StringUtil::CIEquals(words[1], "as")
		                     ? words[1]
		                     : (words.size() > 2 ? words[2] : words[0]);
		auto &catalog_entry = Catalog::GetEntry<TableCatalogEntry>(context, INVALID_CATALOG, INVALID_SCHEMA, words[0]);
		relation.entry = catalog_entry;
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

	StorageSource source(context, bind_data.relations);
	// Bottom-insert is the mode that carries the benefit (FINDINGS F4:
	// bottom-inserts alone are worth 1.9x, top-inserts 0.98x). Phase 3 chooses
	// per join; Phase 2 is about data movement, so it takes the better default.
	// A representation that does not fit is re-counted over a partition of its
	// join key rather than refused. The cap is still honoured -- it is what
	// decides that slicing is needed -- but it now costs passes over the input
	// instead of costing the answer.
	const auto result =
	    factorize::ExecuteCountWithinMemory(bind_data.graph, plan, source, factorize::JoinMode::BOTTOM_INSERT);
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
