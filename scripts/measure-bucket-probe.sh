#!/usr/bin/env bash
# Does this engine's time on ONE bucket separate the queries it should run
# from the ones it should hand back?
#
#   scripts/measure-bucket-probe.sh            # tmp/bucket_probe.csv
#
# D91 ruled out the cost model as the place to fix the gate: the one term
# shown to be measurably wrong was corrected and it surrendered ten good
# outcomes per bad one. D89 proposed per-bucket routing instead, and a probe
# showed the arithmetic works -- eight bucketed stock queries sum to the same
# answer and run slightly faster than one unbucketed one.
#
# What that probe also showed is that the symmetric version cannot work. On a
# query this engine rescues, running the *stock* plan on one bucket costs 73s
# and 83s on the two tried, because an eighth of an explosive join is still an
# explosive join. Exploration must never touch the stock plan.
#
# So this measures the one-sided version: our own wall time on bucket 0, which
# is a bounded fraction of the work and costs a bounded amount to learn. The
# question is whether it separates. The axis is new -- not a predicted cost,
# not a compression ratio, but measured time on a real slice of this query.
#
# `fired` is carried through and the analysis must split on it. Where the gate
# declined, `auto_s` in the verdict file is the stock plan's own time, so a
# query labelled "faster" there may be one this engine never ran -- reading
# those as outcomes of factorizing is how the first pass at this looked
# alarming for no reason.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap CleanSpill EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
N=${N:-8}
CAP=${CAP:-20}
OUT=${OUT:-tmp/bucket_probe.csv}
VERDICTS=${VERDICTS:-tmp/excluded_agg/min.csv}

echo "query,fired,verdict,bucket0_s" > "$OUT"
while IFS=, read -r name fired off auto; do
    [ "$name" = "query" ] && continue
    [ -n "${off:-}" ] || continue
    if [ "$off" = "to" ] && [ "$auto" = "to" ]; then verdict=neither
    elif [ "$off" = "to" ]; then verdict=RESCUE
    elif [ "$auto" = "to" ]; then verdict=lost
    else verdict=$(awk -v a="$auto" -v b="$off" 'BEGIN{print (a<b)?"faster":"slower"}')
    fi
    q=$(grep "^${name}|" tmp/excluded_all.psv | cut -d'|' -f3 | tr -d '\r') || true
    [ -n "$q" ] || continue
    key=$(printf '%s' "$q" | sed -E 's/.* where ([A-Za-z0-9_]+\.[A-Za-z0-9_]+) =.*/\1/')
    case "$key" in *.*) ;; *) continue ;; esac
    start=$(date +%s.%N)
    if printf '%s\n' "SET factorize_mode=${Q}force${Q};" "${q} AND (${key} % ${N}) = 0;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list >/dev/null 2>&1; then
        secs=$(echo "$(date +%s.%N) - ${start}" | bc)
    else
        secs=to
    fi
    echo "${name},${fired},${verdict},${secs}" >> "$OUT"
    echo "  ${name} fired=${fired} ${verdict} bucket0=${secs}" >&2
done < "$VERDICTS"
echo "=== -> $OUT"
