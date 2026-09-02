#!/usr/bin/env python3
"""Parse the FactDB artifact's published CE benchmark logs into a CSV.

The logs under ``bench/results/paper/ce`` in the artifact are the raw numbers
behind the paper's Figures 14 and 15. Parsing them gives us, before writing a
single line of engine code:

  * the exact set of CE queries where stock DuckDB takes >100 ms -- the
    population Phase 1.5's exit and kill criteria are defined over;
  * FactDB's own runtime on those queries, i.e. the target a code-generating
    implementation actually hit;
  * the flat baseline on identical infrastructure, which is how the paper
    isolates the factorization effect from allocator and hash-table differences.

Only measurement logs are read. No engine source is consulted (see DECISIONS.md
D1: the implementation is clean-room from the paper).

Usage:
    python3 scripts/parse_paper_results.py --factdb ../FactDB -o tmp/ce_paper.csv
"""

import argparse
import csv
import os
import re
import sys

# " [ms] execution: (5 warmups, 10 runs, 108.2270145 min, 162.529469 max,
#   139.562845 median, ...)"  -- FactDB's own logs report seconds instead.
SUMMARY = re.compile(
    r"\[(?P<unit>m?s)\]\s+execution:\s*\([^)]*?"
    r"(?P<median>[0-9]+\.[0-9]+)\s+median",
)
QUERYNAME = re.compile(r"^queryname:\s*(?P<name>\S+)\s*$")

#: Log file -> column name. Chosen to mirror the paper's own comparisons.
ENGINES = {
    "duckdb.txt": "duckdb",
    "umbra.txt": "umbra",
    "umbra_le.txt": "umbra_le",
    # Best flat and best factorized configuration: parallel, bushy, inlined,
    # cached. This pair is the honest like-for-like ratio.
    "CodegenFlat_pvbic.txt": "factdb_flat",
    "CodegenFactorized_pvbic.txt": "factdb_factorized",
    # Naive vs improved root-to-leaf merging (paper section 6.2).
    "CodegenFactorized_pvbicn.txt": "factdb_factorized_naive_merge",
    # Ablation ladder, single-threaded left-deep (paper Figure 15).
    "CodegenFlatLeftDeep_v.txt": "abl_flat",
    "CodegenFactorizedLeftDeep_v.txt": "abl_top_insert",
    "CodegenFactorizedLeftDeep_vb.txt": "abl_plus_bottom_insert",
    "CodegenFactorizedLeftDeep_vbc.txt": "abl_plus_caching",
    "CodegenFactorizedLeftDeep_vbic.txt": "abl_plus_inlining",
}


def parse_log(path):
    """Yield (queryname, median_ms) for one log file."""
    current = None
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            name_match = QUERYNAME.match(line)
            if name_match:
                current = name_match.group("name")
                continue
            if current is None:
                continue
            summary = SUMMARY.search(line)
            if not summary:
                continue
            median = float(summary.group("median"))
            if summary.group("unit") == "s":
                median *= 1000.0
            yield current, median
            current = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--factdb", required=True, help="path to the FactDB artifact checkout")
    parser.add_argument("-o", "--output", required=True, help="CSV to write")
    args = parser.parse_args()

    results_dir = os.path.join(args.factdb, "bench", "results", "paper", "ce")
    if not os.path.isdir(results_dir):
        sys.exit("not found: %s" % results_dir)

    # queryname -> {engine: median_ms}
    rows = {}
    present = []
    for filename, engine in ENGINES.items():
        path = os.path.join(results_dir, filename)
        if not os.path.isfile(path):
            print("  skip %-38s (absent)" % filename, file=sys.stderr)
            continue
        count = 0
        for name, median_ms in parse_log(path):
            rows.setdefault(name, {})[engine] = median_ms
            count += 1
        present.append(engine)
        print("  %-38s %6d queries" % (filename, count), file=sys.stderr)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    columns = ["query", "dataset", "shape"] + present
    with open(args.output, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(columns)
        for name in sorted(rows):
            parts = name.split("_")
            dataset = parts[0] if parts else ""
            shape = "cyclic" if "_cyclic" in name else "acyclic" if "_acyclic" in name else "?"
            record = rows[name]
            writer.writerow([name, dataset, shape] + ["%.4f" % record[e] if e in record else "" for e in present])

    print("\nwrote %s: %d queries x %d engines" % (args.output, len(rows), len(present)), file=sys.stderr)


if __name__ == "__main__":
    main()
