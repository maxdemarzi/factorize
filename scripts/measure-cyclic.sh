#!/usr/bin/env bash
# The half of the CE corpus this project has never looked at.
#
#   scripts/measure-cyclic.sh                   # tmp/cyclic/cyclic.csv
#   SAMPLE=200 CAP=60 scripts/measure-cyclic.sh
#
# D96 recorded C -- cyclic queries -- as "no evidence available", because
# measuring them "needs the FactDB query files that are not checked out here".
# They are checked out, at ../FactDB/bench/ce/queries, and D1 permits reading
# benchmark query files. 760 live cyclic queries sit there across dblp,
# epinions, watdiv and yago. (hetio's are commented out in the benchmark
# itself, as are 601 acyclic ones -- that is the corpus's own convention, not
# something applied here.)
#
# Three things are being asked at once, in one pass, because they share the
# expensive part:
#
#   1. CORRECTNESS. `auto` must equal `off` on a cyclic query as on any other.
#      The optimizer rule calls EstimateCost with `acyclic` hardcoded true
#      (optimizer_rule.cpp), so `require_acyclic` -- the threshold that exists
#      to refuse these -- is dead on the DuckDB path. What actually stops a
#      cyclic query is BuildPlan's structural check, which refuses when no
#      relation attaches on keys landing on one f-tree node. That is a
#      different mechanism from the one the comments claim, and whether it
#      holds has never been tested against a cyclic query.
#   2. COVERAGE. How often does the gate fire at all here?
#   3. OPPORTUNITY. `force` says what is behind the declines.
#
# Published result sizes are compared too: all 760 are materialisable (median
# 99 rows, max 11.5M), so unlike the excluded corpus there is an oracle for
# every query.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-60}
LIST=${LIST:-tmp/cyclic/cyclic_sql.psv}
OUT=${OUT:-tmp/cyclic/cyclic.csv}
SAMPLE=${SAMPLE:-0}
SEED=${SEED:-7}

[ -f "$LIST" ] || { echo "no $LIST -- run scripts/ce-to-sql-psv.py first" >&2; exit 1; }
trap 'CleanSpill' EXIT

SEL=$(mktemp); trap 'rm -f "$SEL"; CleanSpill' EXIT
if [ "$SAMPLE" -gt 0 ]; then
    cut -d'|' -f1 "$LIST" | shuf --random-source=<(yes "$SEED") -n "$SAMPLE" | sort >"$SEL"
else
    cut -d'|' -f1 "$LIST" | sort >"$SEL"
fi
echo "=== $(wc -l <"$SEL") of $(wc -l <"$LIST") cyclic queries, cap ${CAP}s"

run_mode() {
    local mode="$1" query="$2" start out
    start=$(date +%s.%N)
    if out=$(printf '%s\n' "SET factorize_mode=${Q}${mode}${Q};" "${query};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1); then
        printf '%s|%s' "$(echo "$(date +%s.%N) - ${start}" | bc)" "$(printf '%s' "$out" | tail -1)"
    else
        # A query that errors and one that times out are different failures and
        # must not be merged: the first says the schema is wrong here, the
        # second says the plan is.
        printf 'to|%s' "$(printf '%s' "${out:-}" | tail -1)"
    fi
}

echo "query,expected,fired,off_s,auto_s,force_s,off_v,auto_v,force_v,verdict" >"$OUT"
fired_n=0 n=0 disagree=0 wrong=0
while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    grep -qxF "$name" "$SEL" || continue
    q=$(printf '%s' "$q" | tr -d '\r')
    n=$((n + 1))
    f=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN ${q};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -ci Factorized || true)
    [ "$f" -gt 0 ] && fired_n=$((fired_n + 1))
    IFS='|' read -r off_s off_v <<<"$(run_mode off "$q")"
    IFS='|' read -r aut_s aut_v <<<"$(run_mode auto "$q")"
    IFS='|' read -r frc_s frc_v <<<"$(run_mode force "$q")"
    verdict=ok
    if [ "$off_s" != to ] && [ "$aut_s" != to ] && [ "$off_v" != "$aut_v" ]; then
        verdict="DISAGREE"; disagree=$((disagree + 1))
    elif [ "$off_s" != to ] && [ "$off_v" != "$expected" ]; then
        # off is stock DuckDB; disagreeing with the published size means the
        # oracle or the load is wrong, not this extension.
        verdict="oracle"; wrong=$((wrong + 1))
    fi
    echo "${name},${expected},${f},${off_s},${aut_s},${frc_s},${off_v},${aut_v},${frc_v},${verdict}" >>"$OUT"
    [ "$verdict" = ok ] || printf '  %-28s %s  off=%s auto=%s force=%s\n' "$name" "$verdict" "$off_v" "$aut_v" "$frc_v"
done <"$LIST"
echo "=== $n measured, $fired_n fired, $disagree auto/off disagreements, $wrong off-vs-published mismatches -> $OUT"
