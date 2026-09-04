//===----------------------------------------------------------------------===//
//                         factorize
//
// duckdb/storage_source.hpp
//
// Reads relations out of DuckDB storage into the core's flat columns.
//
// Shared by both entry points. `factorized_count()` names its relations in
// argument strings; the optimizer rule finds them in a plan DuckDB has already
// bound. From there the scanning, the NULL rule and the statistics are
// identical, and a second implementation of the row-alignment rule in Load()
// would be a second chance to get it wrong -- the first one was a live
// data-corruption bug (DECISIONS D17).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../../core/plan.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

//! One relation: the table it scans, the name it is referenced by, and the
//! columns some predicate touches. Columns no predicate mentions are not read:
//! they cannot change a count, and carrying them would widen every record for
//! nothing.
struct BoundRelation {
	//! Resolved once, when the relation is bound, and held rather than looked up
	//! again at scan time. The optimizer rule takes this entry straight out of a
	//! plan DuckDB has already bound; re-resolving a bare table name against the
	//! search path at execution time could find a *different* table than the one
	//! the query was planned against, and answer a question nobody asked.
	optional_ptr<TableCatalogEntry> entry;
	//! What to call this relation in errors. A self-join is two relations over
	//! one table, so this is not the table name.
	string alias;
	//! Physical column indexes, in the order the f-representation sees them.
	vector<idx_t> columns;
	//! Column name for each entry of `columns`, for error messages.
	vector<string> column_names;
	//! Storage width for each entry of `columns`, parallel to it.
	vector<factorize::ValueType> column_types;
	//! Physical columns scanned only because a pushed-down filter reads them.
	//! They are not join keys: they never reach the engine and need not be
	//! integers. Scanned after `columns`, so filter positions are stable.
	vector<idx_t> filter_columns;
	//! DuckDB's own pushed-down filters, re-keyed to positions in `columns`
	//! followed by `filter_columns`.
	//!
	//! Handed to the storage scan rather than replayed here. The rule replaces
	//! the plan's scans outright, so a filter it failed to apply would not be
	//! applied by anything -- letting DuckDB's own scan evaluate its own filter
	//! set is the only version of this with no second implementation to diverge.
	//! Shared rather than owned because a bound relation is copied from the
	//! logical operator into the physical one.
	shared_ptr<TableFilterSet> filters;

	//! Position of `physical` within `columns`, or -1 if not a join key.
	int LocalIndex(idx_t physical) const;
};

//! The storage width the core must use for an integer column, or false if the
//! type cannot be a join key at all.
//!
//! An f-representation packs keys into fixed-width slots, so a join key has to
//! be an integer of known width. This is not simply "32-bit types get INT32":
//! factorize::INT32 is a *signed* 32-bit slot, and UINTEGER's range
//! (0..4294967295) does not fit in one -- a value above INT32_MAX would be
//! silently truncated. UINTEGER needs INT64 storage despite being nominally
//! 32 bits.
//!
//! Returns a bool rather than throwing because the two callers need opposite
//! things from a bad type: the table function was asked for this relation by
//! name and must say why it cannot, while the optimizer rule must decline the
//! rewrite silently and leave the stock plan alone.
bool TryIntegerKeyType(const LogicalType &type, factorize::ValueType &result);

//! `a(x, y), b(x)`: the relations, with the columns each one contributes.
string DescribeRelations(const vector<BoundRelation> &relations);
//! `a.x = b.x, b.y = c.y`.
string DescribePredicates(const vector<BoundRelation> &relations, const factorize::QueryGraph &graph);
//! `a -> b -> c`: the order the joins run in.
string DescribeJoinOrder(const vector<BoundRelation> &relations, const factorize::Plan &plan);

//! Reads relations out of DuckDB storage on demand.
//!
//! Materialises one relation at a time. The queries this engine exists for have
//! small inputs and enormous results, so holding every base table at once would
//! be the wrong trade even though it would be simpler.
class StorageSource : public factorize::RelationSource {
public:
	StorageSource(ClientContext &context, const vector<BoundRelation> &relations);

	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override;
	factorize::ColumnStats Stats(size_t relation, size_t column) override;

private:
	void Load(size_t relation);

	ClientContext &context;
	const vector<BoundRelation> &relations;
	std::vector<std::vector<int64_t>> held;
	size_t loaded_relation = static_cast<size_t>(-1);
	//! Row offsets surviving the NULL check in the chunk being read. A member
	//! rather than a local so the allocation is made once, not per chunk.
	vector<idx_t> kept;
};

//! Every relation, read once and then read-only, so threads can share it.
//!
//! StorageSource holds one relation at a time, which is the right trade for a
//! single reader. It is the wrong one for several: a thread counting its own
//! bucket of the join key still has to see every row to find its bucket, so N
//! threads over a private StorageSource re-read the whole input N times. On a
//! four-way self-join of 4.5M rows that re-reading *was* the runtime -- eight
//! threads finished no sooner than one, having done eight times the scanning.
class SharedRelations : public factorize::RelationSource {
public:
	SharedRelations(ClientContext &context, const vector<BoundRelation> &relations);

	const std::vector<std::vector<int64_t>> &Columns(size_t relation) override;
	factorize::ColumnStats Stats(size_t relation, size_t column) override;

private:
	std::vector<std::vector<std::vector<int64_t>>> held;
};

} // namespace duckdb
