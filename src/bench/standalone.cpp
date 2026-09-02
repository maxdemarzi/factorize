//===----------------------------------------------------------------------===//
//                         factorize
//
// bench/standalone.cpp
//
// Phase 1.4 harness: runs CE benchmark queries through the factorized core and
// through a flat baseline built on *identical* infrastructure -- the same
// arena, the same chaining hash table -- so the ratio isolates factorization
// rather than allocator or hash-table differences. That is how the paper reports
// its own numbers (plan section 1.4).
//
// Deliberately links no DuckDB (plan section 4). Stock DuckDB is measured
// separately and joined on query name afterwards.
//
// Correctness oracle: every CE query carries its expected result as a
// "-- Result size:" comment, so no reference engine is needed to validate.
//
//===----------------------------------------------------------------------===//

#include "../core/cost.hpp"
#include "../core/join.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace factorize;
using Clock = std::chrono::steady_clock;

static std::string StringLower(std::string value) {
	for (auto &c : value) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return value;
}

static double MillisSince(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

//===--------------------------------------------------------------------===//
// CE corpus
//===--------------------------------------------------------------------===//
struct Predicate {
	size_t left_relation;
	int left_column; // 0 == s, 1 == d
	size_t right_relation;
	int right_column;
};

struct Query {
	std::string name;
	int64_t expected = -1;
	//! Physical table to load, per relation. Several entries may name the same
	//! table: the CE corpus expresses self-joins through aliases
	//! (`from yago2 yago2_2, yago2 yago2_3`), and predicates then reference the
	//! *alias*. Treating "yago2 yago2_2" as one table name makes every such
	//! predicate unresolvable, and the query then looks like a disconnected
	//! join graph rather than a parse failure.
	std::vector<std::string> tables;
	//! Name each relation is referenced by; equals `tables` when unaliased.
	std::vector<std::string> aliases;
	std::vector<Predicate> predicates;
	bool cyclic = false;
};

//! Parses one CE query file. Each file holds many queries, each introduced by
//! `\set queryname`, annotated with `-- Result size:` and terminated by a single
//! `select count(*) ... ;` line.
static std::vector<Query> ParseQueryFile(const std::string &path) {
	std::vector<Query> queries;
	std::ifstream input(path);
	std::string line;
	std::string pending_name;
	int64_t pending_expected = -1;

	// Trailing CR, whitespace and the statement terminator are stripped up front.
	// Leaving them in is not a cosmetic problem: a column reference then reads
	// as "s;" rather than "s", which silently parses as the *other* column and
	// produces a well-formed query with the wrong join predicate.
	auto trim = [](std::string value) {
		while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
			value.erase(value.begin());
		}
		while (!value.empty() &&
		       (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n' ||
		        value.back() == ';')) {
			value.pop_back();
		}
		return value;
	};

	while (std::getline(input, line)) {
		line = trim(line);
		// The epinions files comment the directive out ("--\\set queryname ...")
		// while every other dataset leaves it bare. Accepting only the bare form
		// leaves those queries unnamed, which makes them indistinguishable in the
		// output CSV and invisible to --only.
		const char *name_directive = nullptr;
		if (line.rfind("\\set queryname ", 0) == 0) {
			name_directive = line.c_str() + std::strlen("\\set queryname ");
		} else if (line.rfind("--\\set queryname ", 0) == 0) {
			name_directive = line.c_str() + std::strlen("--\\set queryname ");
		}
		if (name_directive) {
			pending_name = name_directive;
			while (!pending_name.empty() && (pending_name.back() == '\r' || pending_name.back() == ' ')) {
				pending_name.pop_back();
			}
			pending_expected = -1;
			continue;
		}
		if (line.rfind("-- Result size:", 0) == 0) {
			pending_expected = std::strtoll(line.c_str() + std::strlen("-- Result size:"), nullptr, 10);
			continue;
		}
		if (line.rfind("select ", 0) != 0) {
			continue;
		}

		Query query;
		query.name = pending_name;
		query.expected = pending_expected;

		const auto from = line.find(" from ");
		const auto where = line.find(" where ");
		if (from == std::string::npos || where == std::string::npos) {
			continue;
		}
		std::string tables = line.substr(from + 6, where - from - 6);
		std::string conditions = line.substr(where + 7);

		std::unordered_map<std::string, size_t> index_of;
		std::stringstream table_stream(tables);
		std::string token;
		while (std::getline(table_stream, token, ',')) {
			token = trim(token);
			if (token.empty()) {
				continue;
			}
			// "t", "t alias" or "t AS alias". The CE corpus expresses self-joins
			// through aliases, and predicates then reference the alias.
			std::vector<std::string> parts;
			{
				std::stringstream words(token);
				std::string word;
				while (words >> word) {
					if (StringLower(word) != "as") {
						parts.push_back(word);
					}
				}
			}
			if (parts.empty()) {
				continue;
			}
			index_of[parts.size() > 1 ? parts.back() : parts[0]] = query.tables.size();
			query.tables.push_back(parts[0]);
			query.aliases.push_back(parts.size() > 1 ? parts.back() : parts[0]);
		}

		// Conditions are always `t.c = t.c` joined by " and ".
		size_t position = 0;
		while (position < conditions.size()) {
			auto next = conditions.find(" and ", position);
			std::string clause = conditions.substr(position, next == std::string::npos ? std::string::npos
			                                                                           : next - position);
			position = next == std::string::npos ? conditions.size() : next + 5;

			const auto equals = clause.find('=');
			if (equals == std::string::npos) {
				continue;
			}
			const std::string left = trim(clause.substr(0, equals));
			const std::string right = trim(clause.substr(equals + 1));
			const auto left_dot = left.find('.');
			const auto right_dot = right.find('.');
			if (left_dot == std::string::npos || right_dot == std::string::npos) {
				continue;
			}
			auto left_entry = index_of.find(left.substr(0, left_dot));
			auto right_entry = index_of.find(right.substr(0, right_dot));
			if (left_entry == index_of.end() || right_entry == index_of.end()) {
				continue;
			}
			// Anything that is not exactly "s" or "d" is a parse failure, not a
			// column. Defaulting here is how a stray ';' turned `.s` into `.d`
			// and produced a silently wrong -- but perfectly well-formed -- query.
			auto column_of = [&](const std::string &name) {
				if (name == "s") {
					return 0;
				}
				if (name == "d") {
					return 1;
				}
				throw std::runtime_error("unrecognised column '" + name + "' in query " + query.name);
			};
			Predicate predicate {};
			predicate.left_relation = left_entry->second;
			predicate.left_column = column_of(left.substr(left_dot + 1));
			predicate.right_relation = right_entry->second;
			predicate.right_column = column_of(right.substr(right_dot + 1));
			query.predicates.push_back(predicate);
		}

		// A connected graph with more edges than n-1 has a cycle.
		query.cyclic = query.predicates.size() >= query.tables.size();
		if (!query.tables.empty()) {
			queries.push_back(std::move(query));
		}
	}
	return queries;
}

//===--------------------------------------------------------------------===//
// Data
//===--------------------------------------------------------------------===//
//! Entries kept per column in the MCV list. 128 rather than a handful: with
//! only the top-K stored, a value outside the list can only be estimated by the
//! tail average, and hetio needs a deeper list than epinions before that
//! approximation stops dominating the error (FINDINGS F14).
static size_t g_mcv_entries = 128;

struct Table {
	std::vector<int32_t> s;
	std::vector<int32_t> d;
	//! Distinct values per column. Computing it exactly here isolates the gate's
	//! design from the quality of the estimates (plan section 0.6).
	int64_t distinct[2] = {1, 1};
	//! The most common values per column, descending by frequency. This is the
	//! statistic distinct counts cannot substitute for: on skewed join keys the
	//! textbook estimator missed epinions by 1400x and the gate declined every
	//! query on the dataset (FINDINGS F13/F14).
	std::vector<std::pair<int64_t, double>> mcv[2];
};

class TableCache {
public:
	explicit TableCache(std::string directory) : directory(std::move(directory)) {
	}

	const Table &Get(const std::string &name) {
		auto entry = tables.find(name);
		if (entry != tables.end()) {
			return entry->second;
		}
		Table table;
		const std::string path = directory + "/" + name + ".csv";
		std::ifstream input(path, std::ios::binary);
		if (!input) {
			throw std::runtime_error("cannot open " + path);
		}
		std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		const char *cursor = content.c_str();
		const char *end = cursor + content.size();
		while (cursor < end) {
			char *after = nullptr;
			const long first = std::strtol(cursor, &after, 10);
			if (after == cursor) {
				break;
			}
			cursor = after;
			if (cursor < end && *cursor == ',') {
				cursor++;
			}
			const long second = std::strtol(cursor, &after, 10);
			cursor = after;
			while (cursor < end && (*cursor == '\n' || *cursor == '\r')) {
				cursor++;
			}
			table.s.push_back(static_cast<int32_t>(first));
			table.d.push_back(static_cast<int32_t>(second));
		}
		// Distinct counts and MCV lists come out of one sorted pass per column.
		for (int column = 0; column < 2; column++) {
			std::vector<int32_t> scratch(column == 0 ? table.s : table.d);
			std::sort(scratch.begin(), scratch.end());
			int64_t distinct = 0;
			// A min-heap on frequency, so the cheapest entry is the one displaced.
			auto worse = [](const std::pair<int64_t, double> &a, const std::pair<int64_t, double> &b) {
				return a.second > b.second;
			};
			std::vector<std::pair<int64_t, double>> heap;
			for (size_t i = 0; i < scratch.size();) {
				size_t j = i;
				while (j < scratch.size() && scratch[j] == scratch[i]) {
					j++;
				}
				distinct++;
				const std::pair<int64_t, double> entry(scratch[i], static_cast<double>(j - i));
				if (heap.size() < g_mcv_entries) {
					heap.push_back(entry);
					std::push_heap(heap.begin(), heap.end(), worse);
				} else if (!heap.empty() && entry.second > heap.front().second) {
					std::pop_heap(heap.begin(), heap.end(), worse);
					heap.back() = entry;
					std::push_heap(heap.begin(), heap.end(), worse);
				}
				i = j;
			}
			std::sort(heap.begin(), heap.end(),
						  [](const std::pair<int64_t, double> &a, const std::pair<int64_t, double> &b) {
							  return a.second > b.second;
						  });
			table.distinct[column] = std::max<int64_t>(1, distinct);
			table.mcv[column] = std::move(heap);
		}
		bytes += table.s.size() * 2 * sizeof(int32_t);
		return tables.emplace(name, std::move(table)).first->second;
	}

	size_t Bytes() const {
		return bytes;
	}
	void Clear() {
		tables.clear();
		bytes = 0;
	}

private:
	std::string directory;
	std::unordered_map<std::string, Table> tables;
	size_t bytes = 0;
};

//===--------------------------------------------------------------------===//
// Planning
//
// Greedy left-deep order that always takes the relation joined by the *most*
// predicates. That matters beyond cost: when a relation connects by two
// predicates it closes a cycle through a composite key, which forces section
// 4.3 to merge nodes and section 4.6 to partially flatten. Picking such a
// relation early keeps cycles expressible as joins instead of leaving a
// dangling predicate between two already-joined relations, which would need a
// selection operator the core does not have.
//===--------------------------------------------------------------------===//
struct PlanStep {
	size_t relation;
	//! Predicates connecting the new relation to everything joined so far.
	std::vector<Predicate> edges;
};

struct Plan {
	std::vector<PlanStep> steps;
	bool complete = false;
	std::string reason;
};

static Plan BuildPlan(const Query &query) {
	Plan plan;
	std::set<size_t> joined;
	std::vector<bool> used(query.predicates.size(), false);

	// Seed with the relation carrying the first predicate.
	size_t seed = query.predicates.empty() ? 0 : query.predicates[0].left_relation;
	joined.insert(seed);
	plan.steps.push_back(PlanStep {seed, {}});

	while (joined.size() < query.tables.size()) {
		std::map<size_t, std::vector<Predicate>> candidates;
		for (size_t i = 0; i < query.predicates.size(); i++) {
			if (used[i]) {
				continue;
			}
			const auto &predicate = query.predicates[i];
			const bool left_in = joined.count(predicate.left_relation) > 0;
			const bool right_in = joined.count(predicate.right_relation) > 0;
			if (left_in == right_in) {
				continue; // both or neither joined
			}
			candidates[left_in ? predicate.right_relation : predicate.left_relation].push_back(predicate);
		}
		if (candidates.empty()) {
			plan.reason = "join graph is disconnected";
			return plan;
		}
		auto best = candidates.begin();
		for (auto it = candidates.begin(); it != candidates.end(); ++it) {
			if (it->second.size() > best->second.size()) {
				best = it;
			}
		}
		plan.steps.push_back(PlanStep {best->first, best->second});
		joined.insert(best->first);
		for (size_t i = 0; i < query.predicates.size(); i++) {
			const auto &predicate = query.predicates[i];
			if (used[i]) {
				continue;
			}
			const bool covers = (predicate.left_relation == best->first && joined.count(predicate.right_relation)) ||
			                    (predicate.right_relation == best->first && joined.count(predicate.left_relation));
			if (covers) {
				used[i] = true;
			}
		}
	}

	for (size_t i = 0; i < query.predicates.size(); i++) {
		if (!used[i]) {
			// A predicate between two already-joined relations is a selection on
			// the f-representation, which v1 does not implement.
			plan.reason = "residual predicate needs a selection operator";
			return plan;
		}
	}
	plan.complete = true;
	return plan;
}

//===--------------------------------------------------------------------===//
// Factorized execution
//===--------------------------------------------------------------------===//
static AttributeId AttributeOf(size_t relation, int column) {
	return static_cast<AttributeId>(relation * 2 + static_cast<size_t>(column));
}

//===--------------------------------------------------------------------===//
// Equality propagation
//
// `a.s = b.s and b.s = c.s` puts all three attributes in one equivalence class,
// so the second join may be keyed on *a.s* just as correctly as on b.s. Which
// one is chosen decides the f-tree's shape: keying on the most recently added
// relation nests every relation under the last one, producing a chain in which
// nothing is independent and factorization saves nothing. Keying on the
// shallowest available member makes the new subtree a *sibling* of the existing
// ones -- independent, hence a Cartesian product -- which is the shape of the
// paper's running example in Figure 3.
//
// This is ordinary optimizer behaviour, not a trick: DuckDB propagates equality
// the same way.
//===--------------------------------------------------------------------===//
class EquivalenceClasses {
public:
	explicit EquivalenceClasses(const Query &query) {
		parent.resize(query.tables.size() * 2);
		for (size_t i = 0; i < parent.size(); i++) {
			parent[i] = i;
		}
		for (const auto &predicate : query.predicates) {
			Merge(AttributeOf(predicate.left_relation, predicate.left_column),
			      AttributeOf(predicate.right_relation, predicate.right_column));
		}
	}

	size_t Find(size_t x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	}
	bool SameClass(AttributeId a, AttributeId b) {
		return Find(a) == Find(b);
	}
	size_t Size() const {
		return parent.size();
	}

private:
	void Merge(size_t a, size_t b) {
		a = Find(a);
		b = Find(b);
		if (a != b) {
			parent[a] = b;
		}
	}
	std::vector<size_t> parent;
};

//! Rewrites a probe-side key onto the shallowest equivalent attribute already
//! present in the accumulated f-tree.
static AttributeId ShallowestEquivalent(EquivalenceClasses &classes, const FTree &tree, AttributeId key) {
	AttributeId best = key;
	int best_depth = tree.DepthOfAttribute(key);
	for (size_t candidate = 0; candidate < classes.Size(); candidate++) {
		const auto attribute = static_cast<AttributeId>(candidate);
		if (attribute == key || !classes.SameClass(attribute, key)) {
			continue;
		}
		const int depth = tree.DepthOfAttribute(attribute);
		if (depth < 0) {
			continue; // not joined yet
		}
		if (best_depth < 0 || depth < best_depth) {
			best = attribute;
			best_depth = depth;
		}
	}
	return best;
}

//! Builds the gate's view of the plan: for each relation, its size, the
//! distinct counts on both sides of the join that attaches it, and whether it
//! attaches as a sibling. Uses exactly the plan the engine will run, so the
//! prediction describes the query that is actually executed.
static ColumnStats StatsFor(const Table &table, size_t column) {
	ColumnStats stats;
	stats.rows = static_cast<double>(table.s.size());
	stats.distinct = static_cast<double>(table.distinct[column]);
	stats.mcv = table.mcv[column];
	return stats;
}

//! Describes the plan to the gate: one step per relation, carrying its join
//! column's statistics and the equivalence class that column belongs to.
static std::vector<CostStep> BuildCostSteps(const Query &query, const Plan &plan, TableCache &cache) {
	EquivalenceClasses classes(query);

	// Which column each relation joins on. For every relation but the first this
	// is the column its own attaching edge names; the first relation has no
	// attaching edge, so it takes the first edge that mentions it.
	std::map<size_t, size_t> join_column;
	for (size_t i = 1; i < plan.steps.size(); i++) {
		for (const auto &edge : plan.steps[i].edges) {
			join_column.emplace(edge.left_relation, static_cast<size_t>(edge.left_column));
			join_column.emplace(edge.right_relation, static_cast<size_t>(edge.right_column));
		}
	}

	std::vector<CostStep> steps;
	// Mirrors the engine exactly, including equality propagation: a relation
	// attaches beneath the *shallowest* already-joined attribute of its
	// equivalence class, not beneath whichever relation the predicate happens to
	// name. That choice is what turns a chain into a star, and a star is the
	// only shape that compresses -- so an estimator that ignores it predicts a
	// ratio of about 1 for every query, including the ones that compress 3000x.
	std::map<size_t, int> depth_of;   // relation -> depth in the f-tree
	std::map<size_t, size_t> step_of; // relation -> index in `steps`

	const size_t seed_relation = plan.steps[0].relation;
	const auto &seed = cache.Get(query.tables[seed_relation]);
	const size_t seed_column = join_column.count(seed_relation) ? join_column[seed_relation] : 0;
	CostStep first;
	first.key = StatsFor(seed, seed_column);
	first.key_group = static_cast<int>(classes.Find(AttributeOf(seed_relation, static_cast<int>(seed_column))));
	steps.push_back(first);
	depth_of[seed_relation] = 0;
	step_of[seed_relation] = 0;

	for (size_t i = 1; i < plan.steps.size(); i++) {
		const auto &step = plan.steps[i];
		const auto &table = cache.Get(query.tables[step.relation]);
		const auto &edge = step.edges.front();
		const bool left_is_new = edge.left_relation == step.relation;
		const auto new_column = static_cast<size_t>(left_is_new ? edge.left_column : edge.right_column);
		const auto named_relation = left_is_new ? edge.right_relation : edge.left_relation;
		const auto named_column = static_cast<size_t>(left_is_new ? edge.right_column : edge.left_column);
		const auto named_attribute = AttributeOf(named_relation, static_cast<int>(named_column));

		// Find the shallowest joined attribute equivalent to the predicate's.
		size_t attach_relation = named_relation;
		size_t attach_column = named_column;
		int attach_depth = depth_of.count(named_relation) ? depth_of[named_relation] : 0;
		for (size_t candidate = 0; candidate < classes.Size(); candidate++) {
			const auto attribute = static_cast<AttributeId>(candidate);
			const size_t owner = candidate / 2;
			if (!depth_of.count(owner) || !classes.SameClass(attribute, named_attribute)) {
				continue;
			}
			if (depth_of[owner] < attach_depth) {
				attach_depth = depth_of[owner];
				attach_relation = owner;
				attach_column = candidate % 2;
			}
		}

		const auto &attach_table = cache.Get(query.tables[attach_relation]);
		CostStep entry;
		entry.key = StatsFor(table, new_column);
		entry.key_group =
		    static_cast<int>(classes.Find(AttributeOf(step.relation, static_cast<int>(new_column))));
		entry.parent_key = StatsFor(attach_table, attach_column);
		entry.parent_step = static_cast<int>(step_of[attach_relation]);
		steps.push_back(entry);

		depth_of[step.relation] = attach_depth + 1;
		step_of[step.relation] = steps.size() - 1;
	}
	return steps;
}

struct RunResult {
	bool ok = false;
	int64_t count = -1;
	double millis = 0;
	size_t records = 0;
	size_t bytes = 0;
	std::string error;
};

static bool g_verbose = false;
static size_t g_max_frep_bytes = 6ull << 30;
static size_t g_max_table_cache_bytes = 3ull << 30;

static RunResult RunFactorized(const Query &query, const Plan &plan, TableCache &cache, JoinMode mode,
                               PathStrategy strategy) {
	RunResult result;
	const auto start = Clock::now();
	try {
		AttributeTypes types;
		for (size_t r = 0; r < query.tables.size(); r++) {
			types.emplace_back(AttributeOf(r, 0), ValueType::INT32);
			types.emplace_back(AttributeOf(r, 1), ValueType::INT32);
		}

		auto make = [&](size_t relation) {
			const auto &table = cache.Get(query.tables[relation]);
			std::vector<std::vector<int64_t>> columns(2);
			columns[0].assign(table.s.begin(), table.s.end());
			columns[1].assign(table.d.begin(), table.d.end());
			return MakeScan({AttributeOf(relation, 0), AttributeOf(relation, 1)}, types, columns);
		};

		EquivalenceClasses classes(query);
		//! Set by the final join, which counts without materializing.
		int64_t fused_count = -1;
		auto accumulated = make(plan.steps[0].relation);
		for (size_t i = 1; i < plan.steps.size(); i++) {
			const auto &step = plan.steps[i];
			// The accumulated tree must stay on top either way, otherwise the
			// new relation becomes the root and every previous one nests
			// beneath it -- a chain, in which nothing is independent.
			//
			// Top-insert puts the *probe* on top, bottom-insert the *build*, so
			// the two modes take their arguments swapped. That is exactly the
			// paper's  L (top-insert) R == R (bottom-insert) L : the shape is
			// the same, and what differs is which side gets the hash table
			// built on it. Decoupling those two choices is the whole point of
			// having both modes.
			JoinKeys keys;
			std::vector<AttributeId> new_keys;
			std::vector<AttributeId> accumulated_keys;
			for (const auto &edge : step.edges) {
				const bool left_is_new = edge.left_relation == step.relation;
				new_keys.push_back(AttributeOf(left_is_new ? edge.left_relation : edge.right_relation,
				                               left_is_new ? edge.left_column : edge.right_column));
				const auto raw = AttributeOf(left_is_new ? edge.right_relation : edge.left_relation,
				                             left_is_new ? edge.right_column : edge.left_column);
				accumulated_keys.push_back(ShallowestEquivalent(classes, accumulated.Tree(), raw));
			}
			JoinStats join_stats;
			// The aggregate is the topmost operator, so the final join's output
			// exists only to be counted (sections 4.2.2 and 4.5). Building it
			// would be the single most expensive step of the query -- it is
			// where stock DuckDB spends 96% of its time on these shapes.
			const bool last_join = (i + 1 == plan.steps.size());
			if (mode == JoinMode::TOP_INSERT) {
				keys.build = new_keys;
				keys.probe = accumulated_keys;
				if (last_join) {
					fused_count = FactorizedCountJoin(make(step.relation), accumulated, keys, mode, strategy,
					                                  &join_stats);
				} else {
					accumulated =
					    FactorizedJoin(make(step.relation), accumulated, keys, mode, strategy, &join_stats);
				}
			} else {
				keys.build = accumulated_keys;
				keys.probe = new_keys;
				if (last_join) {
					fused_count = FactorizedCountJoin(accumulated, make(step.relation), keys, mode, strategy,
					                                  &join_stats);
				} else {
					accumulated =
					    FactorizedJoin(accumulated, make(step.relation), keys, mode, strategy, &join_stats);
				}
			}
			if (g_verbose) {
				std::fprintf(stderr,
				             "    join %zu: +%s  build_keys=%zu probe_rows=%zu matches=%zu "
				             "records=%zu bytes=%.1fMB count=%lld tree=%s\n",
				             i, query.tables[step.relation].c_str(), join_stats.build_keys, join_stats.probe_rows,
				             join_stats.matches, join_stats.output_records,
				             join_stats.output_bytes / (1024.0 * 1024.0),
				             static_cast<long long>(accumulated.Count()),
				             accumulated.Tree().ToString(DefaultAttributeName).c_str());
			}
		}
		// A single-relation query never reaches a join, so it still counts the
		// scan's own representation.
		result.count = fused_count >= 0 ? fused_count : accumulated.Count();
		result.records = accumulated.Rep().RecordCount();
		result.bytes = accumulated.Rep().BytesAllocated();
		result.ok = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	result.millis = MillisSince(start);
	return result;
}

//===--------------------------------------------------------------------===//
// Flat baseline
//
// Same arena, same chaining hash table, same plan. Intermediates are
// materialized -- that is precisely the cost factorization avoids -- while the
// final join only counts, as any real engine would for count(*).
//===--------------------------------------------------------------------===//
struct FlatRelation {
	std::vector<AttributeId> attributes;
	std::vector<int32_t> values; // row-major
	size_t width = 0;

	size_t Rows() const {
		return width == 0 ? 0 : values.size() / width;
	}
	int Index(AttributeId attribute) const {
		for (size_t i = 0; i < attributes.size(); i++) {
			if (attributes[i] == attribute) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}
};

static RunResult RunFlat(const Query &query, const Plan &plan, TableCache &cache, size_t max_rows) {
	RunResult result;
	const auto start = Clock::now();
	try {
		auto make = [&](size_t relation) {
			const auto &table = cache.Get(query.tables[relation]);
			FlatRelation flat;
			flat.attributes = {AttributeOf(relation, 0), AttributeOf(relation, 1)};
			flat.width = 2;
			flat.values.reserve(table.s.size() * 2);
			for (size_t i = 0; i < table.s.size(); i++) {
				flat.values.push_back(table.s[i]);
				flat.values.push_back(table.d[i]);
			}
			return flat;
		};

		FlatRelation accumulated = make(plan.steps[0].relation);
		int64_t count = 0;

		for (size_t i = 1; i < plan.steps.size(); i++) {
			const auto &step = plan.steps[i];
			FlatRelation build = make(step.relation);

			std::vector<int> build_keys;
			std::vector<int> probe_keys;
			for (const auto &edge : step.edges) {
				const bool left_is_new = edge.left_relation == step.relation;
				const auto build_attribute =
				    AttributeOf(left_is_new ? edge.left_relation : edge.right_relation,
				                left_is_new ? edge.left_column : edge.right_column);
				const auto probe_attribute =
				    AttributeOf(left_is_new ? edge.right_relation : edge.left_relation,
				                left_is_new ? edge.right_column : edge.left_column);
				build_keys.push_back(build.Index(build_attribute));
				probe_keys.push_back(accumulated.Index(probe_attribute));
			}

			auto pack = [](const int32_t *row, const std::vector<int> &columns) {
				uint64_t packed = 0;
				unsigned bit = 0;
				for (auto column : columns) {
					packed |= static_cast<uint64_t>(static_cast<uint32_t>(row[column])) << bit;
					bit += 32;
				}
				return packed;
			};

			ChainingHashTable<uint32_t> table;
			for (size_t row = 0; row < build.Rows(); row++) {
				table.Insert(pack(&build.values[row * build.width], build_keys), static_cast<uint32_t>(row));
			}
			table.Finalize();

			const bool last = (i + 1 == plan.steps.size());
			FlatRelation output;
			std::vector<int> carry;
			if (!last) {
				output.attributes = accumulated.attributes;
				for (size_t c = 0; c < build.attributes.size(); c++) {
					if (accumulated.Index(build.attributes[c]) < 0) {
						output.attributes.push_back(build.attributes[c]);
						carry.push_back(static_cast<int>(c));
					}
				}
				output.width = output.attributes.size();
			}

			for (size_t row = 0; row < accumulated.Rows(); row++) {
				const int32_t *probe_row = &accumulated.values[row * accumulated.width];
				const uint64_t key = pack(probe_row, probe_keys);
				table.ForEachMatch(key, [&](uint32_t build_row) {
					if (last) {
						count++;
						return;
					}
					const int32_t *source = &build.values[build_row * build.width];
					output.values.insert(output.values.end(), probe_row, probe_row + accumulated.width);
					for (auto column : carry) {
						output.values.push_back(source[column]);
					}
				});
				if (!last && output.Rows() > max_rows) {
					throw std::runtime_error("flat intermediate exceeded the row budget");
				}
			}
			if (!last) {
				accumulated = std::move(output);
			}
		}
		result.count = plan.steps.size() == 1 ? static_cast<int64_t>(accumulated.Rows()) : count;
		result.ok = true;
	} catch (const std::exception &error) {
		result.error = error.what();
	}
	result.millis = MillisSince(start);
	return result;
}

//===--------------------------------------------------------------------===//
// Driver
//===--------------------------------------------------------------------===//
int main(int argc, char **argv) {
	std::string data_dir;
	std::vector<std::string> query_files;
	size_t limit = 0;
	size_t max_flat_rows = 200000000;
	int repeats = 1;
	bool run_flat = true;
	std::string calibrate;
	std::string only;

	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		auto next = [&]() { return std::string(argv[++i]); };
		if (arg == "--data") {
			data_dir = next();
		} else if (arg == "--query") {
			query_files.push_back(next());
		} else if (arg == "--limit") {
			limit = std::strtoul(next().c_str(), nullptr, 10);
		} else if (arg == "--max-flat-rows") {
			max_flat_rows = std::strtoul(next().c_str(), nullptr, 10);
		} else if (arg == "--repeats") {
			repeats = std::atoi(next().c_str());
		} else if (arg == "--calibrate") {
			calibrate = next();
		} else if (arg == "--no-flat") {
			run_flat = false;
		} else if (arg == "--max-frep-bytes") {
			g_max_frep_bytes = std::strtoull(next().c_str(), nullptr, 10);
		} else if (arg == "--max-cache-bytes") {
			g_max_table_cache_bytes = std::strtoull(next().c_str(), nullptr, 10);
		} else if (arg == "--verbose") {
			g_verbose = true;
		} else if (arg == "--only") {
			only = next();
		} else {
			std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
			return 2;
		}
	}
	if (data_dir.empty() || query_files.empty()) {
		std::fprintf(stderr,
		             "usage: standalone --data <ce csv dir> --query <file.sql> [--query ...]\n"
		             "                  [--limit N] [--repeats N] [--no-flat] [--only NAME]\n"
		             "                  [--calibrate obs.csv]\n"
		             "                  [--max-flat-rows N]\n");
		return 2;
	}

	// Calibration mode: fit the cost model to observations from this machine
	// instead of inheriting constants fitted on another (DECISIONS O11).
	// Input is a CSV of input_rows,output,millis -- output being records for
	// our engine and result tuples for DuckDB.
	if (!calibrate.empty()) {
		std::ifstream input(calibrate);
		if (!input) {
			std::fprintf(stderr, "cannot open %s\n", calibrate.c_str());
			return 2;
		}
		std::vector<CostSample> samples;
		std::string line;
		while (std::getline(input, line)) {
			CostSample sample;
			if (std::sscanf(line.c_str(), "%lf,%lf,%lf", &sample.input_rows, &sample.output,
			                &sample.millis) == 3 &&
			    sample.millis > 0) {
				samples.push_back(sample);
			}
		}
		if (samples.empty()) {
			std::fprintf(stderr, "no usable samples in %s\n", calibrate.c_str());
			return 2;
		}
		std::printf("quantile,startup_ms,per_input_row_ms,per_output_ms\n");
		for (double quantile : {0.25, 0.5, 0.75, 0.9}) {
			const auto fit = FitEngineCost(samples, quantile);
			std::printf("%.2f,%.6g,%.6g,%.6g\n", quantile, fit.startup_ms, fit.per_input_row_ms,
			            fit.per_output_ms);
		}
		std::fprintf(stderr, "fitted on %zu samples\n", samples.size());
		return 0;
	}

	// Enforced inside the core, at the point of allocation: a single join can
	// blow far past the budget before it returns.
	SetGlobalMemoryLimit(g_max_frep_bytes);

	TableCache cache(data_dir);
	std::printf("query,shape,expected,factorized,flat,fact_top_ms,fact_bottom_ms,flat_ms,records,bytes,predicted_ratio,predicted_flat,gate_fires,status\n");

	size_t executed = 0;
	size_t correct = 0;
	size_t unsupported = 0;

	for (const auto &file : query_files) {
		for (const auto &query : ParseQueryFile(file)) {
			if (!only.empty() && query.name != only) {
				continue;
			}
			if (limit > 0 && executed >= limit) {
				break;
			}
			if (g_verbose) {
				std::fprintf(stderr, "  query %s: tables=[", query.name.c_str());
				for (size_t t = 0; t < query.tables.size(); t++) {
					std::fprintf(stderr, "%s%zu:%s", t ? "," : "", t, query.tables[t].c_str());
				}
				std::fprintf(stderr, "] predicates=[");
				for (size_t p = 0; p < query.predicates.size(); p++) {
					const auto &predicate = query.predicates[p];
					std::fprintf(stderr, "%s%zu.%c=%zu.%c", p ? "," : "", predicate.left_relation,
					             predicate.left_column ? 'd' : 's', predicate.right_relation,
					             predicate.right_column ? 'd' : 's');
				}
				std::fprintf(stderr, "]\n");
			}
			if (cache.Bytes() > g_max_table_cache_bytes) {
				cache.Clear();
			}
			const auto plan = BuildPlan(query);
			if (g_verbose && plan.complete) {
				std::fprintf(stderr, "  plan: ");
				for (size_t s = 0; s < plan.steps.size(); s++) {
					std::fprintf(stderr, "%s%s", s ? " -> " : "", query.tables[plan.steps[s].relation].c_str());
				}
				std::fprintf(stderr, "\n");
			}
			if (!plan.complete) {
				unsupported++;
				std::printf("%s,%s,%lld,,,,,,,,unsupported: %s\n", query.name.c_str(),
				            query.cyclic ? "cyclic" : "acyclic", static_cast<long long>(query.expected),
				            plan.reason.c_str());
				std::fflush(stdout);
				continue;
			}

			// The first repeat is a warmup; the reported time is the median of
			// the rest. Keeping only the last run, as an earlier version did,
			// reports one arbitrary sample and makes `--repeats` look like it
			// improves the measurement while doing nothing of the sort.
			auto measure = [&](const std::function<RunResult()> &run) {
				RunResult result;
				std::vector<double> samples;
				const int total = repeats > 1 ? repeats : 1;
				for (int r = 0; r < total; r++) {
					result = run();
					if (total == 1 || r > 0) {
						samples.push_back(result.millis);
					}
				}
				std::sort(samples.begin(), samples.end());
				result.millis = samples[samples.size() / 2];
				return result;
			};

			RunResult top, bottom, flat;
			top = measure([&]() {
				return RunFactorized(query, plan, cache, JoinMode::TOP_INSERT, PathStrategy::LEVELWISE);
			});
			bottom = measure([&]() {
				return RunFactorized(query, plan, cache, JoinMode::BOTTOM_INSERT, PathStrategy::LEVELWISE);
			});
			if (run_flat) {
				flat = measure([&]() { return RunFlat(query, plan, cache, max_flat_rows); });
			}

			executed++;
			std::string status = "ok";
			const int64_t reference = query.expected;
			if (!top.ok) {
				status = "factorized error: " + top.error;
			} else if (!bottom.ok) {
				status = "bottom error: " + bottom.error;
			} else if (top.count != bottom.count) {
				status = "MISMATCH between insert modes";
			} else if (reference >= 0 && top.count != reference) {
				status = "MISMATCH vs oracle";
			} else if (run_flat && flat.ok && flat.count != top.count) {
				status = "MISMATCH vs flat";
			} else if (run_flat && !flat.ok) {
				status = "ok (flat: " + flat.error + ")";
			} else {
				correct++;
			}

			// The gate runs on the same plan the engine executed, so the
			// prediction describes the query that was actually measured.
			CostEstimate gate;
			try {
				const auto cost_steps = BuildCostSteps(query, plan, cache);
			if (g_verbose) {
				std::fprintf(stderr, "  cost steps:\n");
				for (size_t c = 0; c < cost_steps.size(); c++) {
					const auto &cs = cost_steps[c];
					std::fprintf(stderr, "    [%zu] group=%d parent=%d rows=%.0f distinct=%.0f mcv=%zu parent_distinct=%.0f\n",
					             c, cs.key_group, cs.parent_step, cs.key.rows, cs.key.distinct,
					             cs.key.mcv.size(), cs.parent_key.distinct);
				}
			}
			gate = EstimateCost(cost_steps, !query.cyclic);
			} catch (const std::exception &) {
				gate.reason = "estimate failed";
			}

			std::printf("%s,%s,%lld,%lld,%lld,%.3f,%.3f,%.3f,%zu,%zu,%.4f,%.0f,%d,%s\n", query.name.c_str(),
			            query.cyclic ? "cyclic" : "acyclic", static_cast<long long>(reference),
			            static_cast<long long>(top.count),
			            static_cast<long long>(run_flat && flat.ok ? flat.count : -1), top.millis, bottom.millis,
			            run_flat && flat.ok ? flat.millis : -1.0, top.records, top.bytes, gate.ratio,
			            gate.flat_tuples, gate.fire ? 1 : 0, status.c_str());
			std::fflush(stdout);
		}
	}

	std::fprintf(stderr, "\nexecuted %zu, correct %zu, unsupported %zu, table cache %.1f MB\n", executed, correct,
	             unsupported, cache.Bytes() / (1024.0 * 1024.0));
	return 0;
}
