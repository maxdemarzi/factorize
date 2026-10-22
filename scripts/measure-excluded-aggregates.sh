#!/usr/bin/env bash
# Do the aggregates behave on the excluded corpus the way count(*) does?
#
#   scripts/measure-excluded-aggregates.sh            # tmp/excluded_agg/<kind>.csv
#   KINDS="min sum" SAMPLE=60 CAP=30 scripts/measure-excluded-aggregates.sh
#
# Every per-fold number this project has is from the 119 runnable queries --
# the regime where the extension is *least* useful, since DuckDB answers those
# already. The 481 excluded queries are where it earns its keep: 244 of them
# have no answer without it. Those have only ever been measured with count(*),
# so whether min or sum or sum(x * y) rescues the same queries has been assumed
# rather than known since the aggregates shipped (D81, D86).
#
# The first version of this timed only `auto`, reasoning that a query
# classified RESCUE under count(*) has a stock plan that times out
# materialising more than 1e9 tuples, and that changing the fold cannot change
# that. The spot-check written to confirm the assumption refuted it on its
# first two queries: stock finishes `min` within 30s on dblp_acyclic_205_07,
# which it cannot do for count(*). **The excluded corpus is defined by count(*)
# behaviour and does not transfer.** So both modes are timed, and the cost is
# paid for with a sample instead.
#
# One sample, drawn once and shared by every fold, because the comparison is
# between folds and that needs them measured on the same queries.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-60}
KINDS=${KINDS:-"min max avg sum sumprod"}
DIR=${DIR:-tmp/excluded_agg}
SAMPLE=${SAMPLE:-120}
SEED=${SEED:-7}

SAMPLE_LIST=$(mktemp)
trap 'rm -f "$SAMPLE_LIST"; CleanSpill' EXIT
cut -d'|' -f1 "$DIR/agg_min.psv" | shuf --random-source=<(yes "$SEED") -n "$SAMPLE" | sort >"$SAMPLE_LIST"
echo "=== sampling $(wc -l <"$SAMPLE_LIST") of $(wc -l <"$DIR/agg_min.psv") queries, cap ${CAP}s"

run_mode() {
    # Wall clock rather than .timer, because a timeout has to be reported as
    # one and `.timer on` prints nothing when the process is killed.
    local mode="$1" query="$2" start
    start=$(date +%s.%N)
    if printf '%s\n' "SET factorize_mode=${Q}${mode}${Q};" "${query};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list >/dev/null 2>&1; then
        echo "$(date +%s.%N) - ${start}" | bc
    else
        printf 'to'
    fi
}

for kind in $KINDS; do
    list="$DIR/agg_${kind}.psv"
    out="$DIR/${kind}.csv"
    [ -f "$list" ] || { echo "no $list" >&2; continue; }
    echo "query,fired,off_s,auto_s" >"$out"
    fired=0 n=0
    while IFS='|' read -r name expected q; do
        [ -n "$name" ] || continue
        grep -qxF "$name" "$SAMPLE_LIST" || continue
        q=$(printf '%s' "$q" | tr -d '\r')
        n=$((n + 1))
        f=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN ${q};" |
            timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -ci Factorized || true)
        [ "$f" -gt 0 ] && fired=$((fired + 1))
        off=$(run_mode off "$q")
        auto=$(run_mode auto "$q")
        echo "${name},${f},${off},${auto}" >>"$out"
    done <"$list"
    echo "=== $kind: $n sampled, $fired fire -> $out"
done
