#include "factorize/storage_source.hpp"

#include "duckdb/common/column_index.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>

namespace duckdb {

//! Matches the harness (DECISIONS D13): measured across five datasets, 128
//! entries is where hetio's error settles and more buys almost nothing.
static constexpr size_t MCV_ENTRIES = 128;

int BoundRelation::LocalIndex(idx_t physical) const {
	for (idx_t i = 0; i < columns.size(); i++) {
		if (columns[i] == physical) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool TryIntegerKeyType(const LogicalType &type, factorize::ValueType &result) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
		result = factorize::ValueType::INT32;
		return true;
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
		result = factorize::ValueType::INT64;
		return true;
	default:
		return false;
	}
}

//! EXPLAIN is where a takeover is either visible or invisible, and an operator
//! that silently replaced half the plan had better say what it replaced it with
//! (plan item 3.6).
string DescribeRelations(const vector<BoundRelation> &relations) {
	string result;
	for (auto &relation : relations) {
		if (!result.empty()) {
			result += ", ";
		}
		result += relation.alias + "(" + StringUtil::Join(relation.column_names, ", ") + ")";
	}
	return result;
}

string DescribePredicates(const vector<BoundRelation> &relations, const factorize::QueryGraph &graph) {
	string result;
	for (auto &predicate : graph.predicates) {
		auto &left = relations[predicate.left_relation];
		auto &right = relations[predicate.right_relation];
		if (!result.empty()) {
			result += ", ";
		}
		result += left.alias + "." + left.column_names[predicate.left_column] + " = " + right.alias + "." +
		          right.column_names[predicate.right_column];
	}
	return result;
}

string DescribeJoinOrder(const vector<BoundRelation> &relations, const factorize::Plan &plan) {
	if (!plan.complete) {
		return plan.reason;
	}
	string result;
	for (auto &step : plan.steps) {
		if (!result.empty()) {
			result += " -> ";
		}
		result += relations[step.relation].alias;
	}
	return result;
}

//! Appends the surviving rows of one column, deciding the type once for the
//! whole batch rather than once per value.
template <class T>
static void AppendTyped(const UnifiedVectorFormat &format, const vector<idx_t> &kept, std::vector<int64_t> &out) {
	const auto *values = UnifiedVectorFormat::GetData<T>(format);
	out.reserve(out.size() + kept.size());
	for (auto row : kept) {
		out.push_back(static_cast<int64_t>(values[format.sel->get_index(row)]));
	}
}

static void AppendColumn(const LogicalType &type, const UnifiedVectorFormat &format, const vector<idx_t> &kept,
                         std::vector<int64_t> &out) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return AppendTyped<int8_t>(format, kept, out);
	case LogicalTypeId::SMALLINT:
		return AppendTyped<int16_t>(format, kept, out);
	case LogicalTypeId::INTEGER:
		return AppendTyped<int32_t>(format, kept, out);
	case LogicalTypeId::BIGINT:
		return AppendTyped<int64_t>(format, kept, out);
	case LogicalTypeId::UTINYINT:
		return AppendTyped<uint8_t>(format, kept, out);
	case LogicalTypeId::USMALLINT:
		return AppendTyped<uint16_t>(format, kept, out);
	case LogicalTypeId::UINTEGER:
		return AppendTyped<uint32_t>(format, kept, out);
	case LogicalTypeId::UBIGINT:
		return AppendTyped<uint64_t>(format, kept, out);
	default:
		throw InternalException("factorize: unsupported key type %s", type.ToString());
	}
}


StorageSource::StorageSource(ClientContext &context_p, const vector<BoundRelation> &relations_p)
    : context(context_p), relations(relations_p) {
}

const std::vector<std::vector<int64_t>> &StorageSource::Columns(size_t relation) {
	if (loaded_relation == relation && !held.empty()) {
		return held;
	}
	Load(relation);
	loaded_relation = relation;
	return held;
}

factorize::ColumnStats StorageSource::Stats(size_t relation, size_t column) {
	const auto &columns = Columns(relation);
	factorize::ColumnStats stats;
	stats.rows = static_cast<double>(columns[column].size());
	// Exact statistics, computed from the data just read. DuckDB's catalog
	// carries approximate distinct counts and no MCV list at all, so a gate that
	// has to decide *before* scanning must sample instead -- see DECISIONS D13.
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

void StorageSource::Load(size_t relation) {
	const auto &bound = relations[relation];
	// Copied rather than dereferenced in place: `bound` is const, so
	// optional_ptr's const operator* would hand back a const entry, and reading
	// a table's storage is a non-const operation.
	auto entry_ptr = bound.entry;
	auto &entry = *entry_ptr;
	auto &storage = entry.GetStorage();
	auto &transaction = DuckTransaction::Get(context, entry.catalog);

	vector<StorageIndex> column_ids;
	vector<ColumnIndex> column_indexes;
	vector<LogicalType> types;
	// Join keys first, then whatever a filter needs to read. The filter set is
	// keyed by position in exactly this order.
	auto scan_column = [&](idx_t physical) {
		auto &definition = entry.GetColumns().GetColumn(PhysicalIndex(physical));
		column_ids.emplace_back(physical);
		column_indexes.emplace_back(definition.Logical().index);
		types.push_back(definition.Type());
	};
	for (auto physical : bound.columns) {
		scan_column(physical);
	}
	for (auto physical : bound.filter_columns) {
		scan_column(physical);
	}

	held.assign(bound.columns.size(), {});

	// The parallel entry points, which is what DuckDB's own sequential scan uses
	// -- not DataTable::InitializeScan, which asserts on a table that has no row
	// groups at all. Scanning an empty table is not an edge case worth a special
	// case: an empty relation empties the whole join, and asking for that count
	// is ordinary. (It also crashed the table function, on origin/main, from
	// Phase 2 onwards; the CE corpus simply has no empty tables.) These entry
	// points are also the ones that see rows still sitting in the transaction's
	// local storage, uncommitted.
	TableScanState state;
	state.Initialize(column_ids, context, bound.filters.get());
	ParallelTableScanState parallel;
	storage.InitializeParallelScan(context, parallel, column_indexes);
	if (storage.NextParallelScan(context, parallel, state) == 0) {
		return;
	}

	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), types);
	while (true) {
		chunk.Reset();
		storage.Scan(transaction, chunk, state);
		if (chunk.size() == 0) {
			if (storage.NextParallelScan(context, parallel, state) == 0) {
				break;
			}
			continue;
		}
		// A relation's columns must stay row-aligned: held[0][k] and held[1][k]
		// have to describe the same source row, because MakeScan zips them back
		// together by shared index with no row id attached. Deciding a row's fate
		// (kept or dropped) requires looking at every column *before* pushing any
		// of them -- checking and pushing one column at a time, independently,
		// drops rows from whichever columns happen to hold a NULL and
		// desynchronizes every row after the first such NULL for a relation with
		// more than one join column. This was a live bug (DECISIONS D17): filter
		// once per row, across all columns, or not at all.
		std::vector<UnifiedVectorFormat> formats(bound.columns.size());
		for (idx_t c = 0; c < bound.columns.size(); c++) {
			chunk.data[c].ToUnifiedFormat(chunk.size(), formats[c]);
		}
		// Which rows survive is decided first, for the whole chunk, so that the
		// per-column work below can be a tight typed loop instead of a switch
		// per value. Reading 18 million rows a row at a time, re-deciding the
		// column's type for each one, was most of the runtime of a query whose
		// join is supposed to be the expensive part.
		kept.clear();
		for (idx_t row = 0; row < chunk.size(); row++) {
			bool all_valid = true;
			for (idx_t c = 0; c < bound.columns.size() && all_valid; c++) {
				all_valid = formats[c].validity.RowIsValid(formats[c].sel->get_index(row));
			}
			if (all_valid) {
				kept.push_back(row);
			}
			// A NULL in any of this relation's join columns can never equal
			// anything, so the row can never contribute to an inner join's count
			// -- and it is dropped from every column at once, never one column at
			// a time, which is the rule the row alignment above depends on.
		}
		for (idx_t c = 0; c < bound.columns.size(); c++) {
			AppendColumn(chunk.data[c].GetType(), formats[c], kept, held[c]);
		}
	}
}

SharedRelations::SharedRelations(ClientContext &context, const vector<BoundRelation> &relations) {
	StorageSource source(context, relations);
	held.reserve(relations.size());
	for (size_t relation = 0; relation < relations.size(); relation++) {
		// Copied out, because StorageSource overwrites its buffer on the next
		// relation. The copy is the price of reading each relation once instead
		// of once per thread.
		held.push_back(source.Columns(relation));
	}
}

const std::vector<std::vector<int64_t>> &SharedRelations::Columns(size_t relation) {
	return held[relation];
}

factorize::ColumnStats SharedRelations::Stats(size_t relation, size_t column) {
	factorize::ColumnStats stats;
	stats.rows = static_cast<double>(held[relation][column].size());
	stats.distinct = stats.rows > 0 ? stats.rows : 1;
	return stats;
}

} // namespace duckdb
