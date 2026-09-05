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
	//! How many tuples factorized_tuples should emit; 0 means all of them.
	size_t limit = 0;
	//! Which columns factorized_group_count groups on, in the order given.
	vector<factorize::GroupKey> group_keys;
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

//! Splits on commas and trims, so "a.x, b.y" names two columns.
vector<string> SplitCommas(const string &text) {
	vector<string> parts;
	size_t start = 0;
	while (true) {
		const auto comma = text.find(',', start);
		const auto end = comma == string::npos ? text.size() : comma;
		auto part = Trim(text.substr(start, end - start));
		if (!part.empty()) {
			parts.push_back(std::move(part));
		}
		if (comma == string::npos) {
			break;
		}
		start = comma + 1;
	}
	return parts;
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

	// Two lists, and whatever the caller adds after them: factorized_tuples
	// takes a limit and factorized_group_count a grouping column, both of which
	// they read themselves once the relations are bound.
	if (input.inputs.size() < 2) {
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
	//! Grouping produces a row per distinct value, which can outrun one vector,
	//! so the groups are computed once and handed out across calls.
	unique_ptr<std::vector<std::pair<std::vector<int64_t>, std::vector<int64_t>>>> groups;
	idx_t offset = 0;
};

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<FactorizedCountGlobalState>();
}

//! Shared by both entry points, which differ only in the question they ask of
//! the same join: how many, or whether any.
template <bool EXISTS>
void ExecuteQuestion(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	const char *name = EXISTS ? "factorized_exists" : "factorized_count";
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
		throw InvalidInputException("%s: %s", name, plan.reason);
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
	    EXISTS ? factorize::ExecuteExists(bind_data.graph, plan, source, factorize::JoinMode::BOTTOM_INSERT)
	           : factorize::ExecuteCountWithinMemory(bind_data.graph, plan, source, factorize::JoinMode::BOTTOM_INSERT);
	if (!result.ok) {
		throw InvalidInputException("%s: %s", name, result.error);
	}

	output.SetCardinality(1);
	if (EXISTS) {
		output.SetValue(0, 0, Value::BOOLEAN(result.count > 0));
	} else {
		output.SetValue(0, 0, Value::BIGINT(result.count));
	}
}

unique_ptr<FunctionData> BindExists(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = Bind(context, input, return_types, names);
	return_types.clear();
	names.clear();
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("exists");
	return result;
}

//! `factorized_tuples(tables, joins[, limit])`: the join itself, not an
//! aggregate of it. One column per join column, named `<relation>_<column>`.
unique_ptr<FunctionData> BindTuples(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = Bind(context, input, return_types, names);
	auto &bind_data = result->Cast<FactorizedCountBindData>();
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		const auto limit = input.inputs[2].GetValue<int64_t>();
		if (limit < 0) {
			throw BinderException("factorized_tuples: limit must not be negative");
		}
		bind_data.limit = static_cast<size_t>(limit);
	}
	return_types.clear();
	names.clear();
	for (auto &relation : bind_data.relations) {
		for (auto &column : relation.column_names) {
			// Only the join columns exist in the representation: a column no
			// predicate mentions was never read, because it cannot change a
			// count and carrying it would widen every record for nothing. That
			// trade is right for counting and is the reason this is not
			// `SELECT *`.
			return_types.emplace_back(LogicalType::BIGINT);
			names.emplace_back(relation.alias + "_" + column);
		}
	}
	return result;
}

//! `factorized_group_count(tables, joins, 'r.a, s.b')`: one row per distinct
//! combination of the grouping columns, with the number of joined tuples it
//! accounts for. Several keys are answered by grouping each branch of the
//! f-tree that carries one and combining the branches, so they need not sit
//! together or near the root.
unique_ptr<FunctionData> BindGroupCount(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto result = Bind(context, input, return_types, names);
	auto &bind_data = result->Cast<FactorizedCountBindData>();
	if (input.inputs.size() < 3 || input.inputs[2].IsNull()) {
		throw BinderException("factorized_group_count: needs a grouping column");
	}
	return_types.clear();
	names.clear();
	const auto operands = SplitCommas(StringValue::Get(input.inputs[2]));
	if (operands.empty()) {
		throw BinderException("factorized_group_count: needs a grouping column");
	}
	for (const auto &operand : operands) {
		const auto dot = operand.rfind('.');
		if (dot == string::npos) {
			throw BinderException("factorized_group_count: '%s' must be <relation>.<column>", operand);
		}
		const auto alias = operand.substr(0, dot);
		const auto column = operand.substr(dot + 1);
		bool found = false;
		for (idx_t relation = 0; relation < bind_data.relations.size() && !found; relation++) {
			auto &bound = bind_data.relations[relation];
			if (bound.alias != alias) {
				continue;
			}
			for (idx_t i = 0; i < bound.column_names.size(); i++) {
				if (bound.column_names[i] == column) {
					bind_data.group_keys.push_back(factorize::GroupKey {relation, i});
					found = true;
					break;
				}
			}
			if (!found) {
				// The column may exist on the table and simply not be read: only
				// the columns a predicate touches are scanned, so grouping on any
				// other would need a column the representation does not hold.
				// (The optimizer rule adds such a column to the scan; this
				// function takes the relations exactly as they were listed.)
				throw BinderException("factorized_group_count: %s.%s is not a join column of this query", alias,
				                      column);
			}
		}
		if (!found) {
			throw BinderException("factorized_group_count: '%s' does not name a listed relation", alias);
		}
		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back(alias + "_" + column);
	}
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("count");
	return result;
}

void ExecuteGroupCount(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FactorizedCountGlobalState>();
	auto &bind_data = data.bind_data->Cast<FactorizedCountBindData>();
	if (!state.groups) {
		const auto &config = DBConfig::GetConfig(context);
		const idx_t available = config.options.maximum_memory == DConstants::INVALID_INDEX
		                            ? static_cast<idx_t>(4) * 1024 * 1024 * 1024
		                            : config.options.maximum_memory;
		factorize::SetGlobalMemoryLimit(static_cast<size_t>(available / 2));

		const auto plan = factorize::BuildPlan(bind_data.graph);
		if (!plan.complete) {
			throw InvalidInputException("factorized_group_count: %s", plan.reason);
		}
		StorageSource source(context, bind_data.relations);
		auto result = factorize::ExecuteGroupCount(bind_data.graph, plan, source, factorize::JoinMode::BOTTOM_INSERT,
		                                           bind_data.group_keys);
		if (!result.ok) {
			throw InvalidInputException("factorized_group_count: %s", result.error);
		}
		state.groups = make_uniq<std::vector<std::pair<std::vector<int64_t>, std::vector<int64_t>>>>(std::move(result.groups));
	}

	// Groups can outnumber a vector, so this one hands them out a chunk at a
	// time rather than refusing past the first.
	auto &groups = *state.groups;
	idx_t produced = 0;
	while (state.offset < groups.size() && produced < STANDARD_VECTOR_SIZE) {
		const auto &group = groups[state.offset];
		for (idx_t key = 0; key < group.first.size(); key++) {
			output.SetValue(key, produced, Value::BIGINT(group.first[key]));
		}
		output.SetValue(group.first.size(), produced, Value::BIGINT(group.second[0]));
		state.offset++;
		produced++;
	}
	output.SetCardinality(produced);
}

void ExecuteTuples(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<FactorizedCountGlobalState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	state.emitted = true;

	auto &bind_data = data.bind_data->Cast<FactorizedCountBindData>();
	const auto &config = DBConfig::GetConfig(context);
	const idx_t available = config.options.maximum_memory == DConstants::INVALID_INDEX
	                            ? static_cast<idx_t>(4) * 1024 * 1024 * 1024
	                            : config.options.maximum_memory;
	factorize::SetGlobalMemoryLimit(static_cast<size_t>(available / 2));

	const auto plan = factorize::BuildPlan(bind_data.graph);
	if (!plan.complete) {
		throw InvalidInputException("factorized_tuples: %s", plan.reason);
	}

	StorageSource source(context, bind_data.relations);
	auto result = factorize::ExecuteMaterializeWithinMemory(bind_data.graph, plan, source,
	                                                       factorize::JoinMode::BOTTOM_INSERT, bind_data.limit);
	if (!result.ok) {
		throw InvalidInputException("factorized_tuples: %s", result.error);
	}
	if (result.tuples.size() > STANDARD_VECTOR_SIZE) {
		// One chunk, so the caller has to ask for an amount that fits in one.
		// Streaming this properly means holding the representation across calls
		// and an enumerator that can be resumed mid-tuple; the limit is the
		// interesting case (plan §10.2) and it does fit.
		throw InvalidInputException(
		    "factorized_tuples: %llu tuples exceeds one vector; pass a limit of %llu or fewer",
		    static_cast<uint64_t>(result.tuples.size()), static_cast<uint64_t>(STANDARD_VECTOR_SIZE));
	}

	output.SetCardinality(result.tuples.size());
	for (idx_t row = 0; row < result.tuples.size(); row++) {
		const auto &tuple = result.tuples[row];
		for (idx_t column = 0; column < tuple.size(); column++) {
			output.SetValue(column, row, Value::BIGINT(tuple[column]));
		}
	}
}

} // namespace

void RegisterFactorizedCount(ExtensionLoader &loader) {
	// Asking whether a join has any tuple is a cheaper question than asking how
	// many, and the engine can stop as soon as it knows (plan §10.4).
	TableFunction exists("factorized_exists",
	                     {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)},
	                     ExecuteQuestion<true>, BindExists, InitGlobal);
	loader.RegisterFunction(exists);

	// The join itself rather than an aggregate of it (plan §10.3), and with a
	// limit, the thing DuckDB cannot do at any speed (§10.2): a hundred rows out
	// of a join with a trillion, without materialising the trillion.
	TableFunction tuples("factorized_tuples",
	                     {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)},
	                     ExecuteTuples, BindTuples, InitGlobal);
	loader.RegisterFunction(tuples);

	TableFunction limited_tuples(
	    "factorized_tuples",
	    {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BIGINT},
	    ExecuteTuples, BindTuples, InitGlobal);
	loader.RegisterFunction(limited_tuples);

	// One row per group, counted without enumerating the tuples in it (§10.1).
	TableFunction group_count(
	    "factorized_group_count",
	    {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR},
	    ExecuteGroupCount, BindGroupCount, InitGlobal);
	loader.RegisterFunction(group_count);

	TableFunction function("factorized_count",
	                       {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)},
	                       ExecuteQuestion<false>, Bind, InitGlobal);
	loader.RegisterFunction(function);
}

} // namespace duckdb
