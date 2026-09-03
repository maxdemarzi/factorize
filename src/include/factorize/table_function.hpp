//===----------------------------------------------------------------------===//
//                         factorize
//
// factorize/table_function.hpp
//
// `factorized_count(tables, joins)` — the explicit entry point.
//
// Phase 2 of the plan, and deliberately the boring half of the integration: it
// connects the core to real DuckDB storage with no optimizer involvement, so
// that when something is wrong it is obvious whether the fault is in reading
// data or in choosing to. Phase 3's optimizer rule reuses everything here
// except the argument parsing.
//
//   SELECT * FROM factorized_count(
//       ['dblp20', 'dblp17', 'dblp25'],
//       ['dblp20.s = dblp17.s', 'dblp17.s = dblp25.s']
//   );
//
// Self-joins are written the way SQL writes them, with an alias:
//
//       ['yago2 a', 'yago2 b'], ['a.s = b.d']
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Registers `factorized_count`.
void RegisterFactorizedCount(ExtensionLoader &loader);

} // namespace duckdb
