# Errata against `tmp/DUCKDB_EXTENSION_PLAN.md`

Corrections found while implementing. Each is checked against the paper text
(`p3006-lehner.pdf`), not against the FactDB artifact (see DECISIONS.md D1).

---

## E1 — §2.2 states top- and bottom-insert backwards ⚠️ **load-bearing**

The plan says:

> - **Top-insert:** probe tree goes *below* build tree.
> - **Bottom-insert:** probe tree becomes the *upper* part.

Both are inverted. The paper, §4.2.3:

> "we introduce both approaches as top- and bottom-insert, **depending on the
> insert position of the probe side**"

- **Top-insert** — the **probe** tree is the **upper** part; the **build** tree is
  inserted below it:
  > "we always place the build tree in top-inserts as a child of the joined node
  > from the probe side."

  Corroborated by §4.4: *"Before traversing the matches, the upper part of the
  output tree is generated from `T_probe`. Then, each matching tuple from the
  build side is inserted into this tree."*

- **Bottom-insert** — the **build** tree is the **upper** part; the **probe** tree
  is inserted below it:
  > "The second option for joining f-trees is to place the probe f-tree below the
  > build side. […] But now, the build f-tree becomes the tree's upper part."

The mnemonic is the probe side's position: top-insert puts the probe on top,
bottom-insert puts it on the bottom.

**Why it matters.** Getting this backwards inverts the parent/child relationship
of every join in every f-tree, which silently destroys the factorization it is
supposed to create — and it would also invert the Phase 3 heuristic, which is
motivated in §4.2.3 by exactly this asymmetry: with a small unique build side `B`
and a large duplicate-heavy probe side `P`, you want `B` in the upper tree so its
values are shared, i.e. `P` lower, i.e. **bottom-insert**.

**Note:** the plan's own `mergeTrees` code excerpt in the same section is
*correct* — with `left = build`, `right = probe`, it selects
`upper = BottomInsert ? left(build) : right(probe)`, matching the paper. Only the
surrounding prose is inverted. The implementation follows the paper and the
excerpt, not the prose.

---

## E2 — §3.2 API signatures are stale for the pinned DuckDB v1.5.5

Recorded in DECISIONS.md under "Phase 0 findings". Summary: `CreatePlan` returns
`PhysicalOperator &` (arena-allocated via `planner.Make<T>()`), not
`unique_ptr<PhysicalOperator>`; `PhysicalOperator::children` holds references;
the source override point is the protected `GetDataInternal`.

---

## E3 — §8.2/§Phase 1 reference a repository that is not present

The plan is written as if the FactDB artifact sits alongside it ("this repository
is GPLv3", `factDB/newftree/FTree.cpp`, `bench/ce/queries/*.sql`). This repository
is a clean DuckDB extension template; none of those paths exist. Under
DECISIONS.md D1 (path B) they are deliberately *not* fetched, so every §8.2
pointer is unusable by construction and the paper is the sole source.

Consequence for Phase 1.4: the CE corpus and its embedded `-- Result size:`
oracles are also unavailable and must be obtained separately
(`./bench/ce/setup.sh` from the artifact, or regenerated). Tracked as O5.
