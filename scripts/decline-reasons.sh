#!/usr/bin/env bash
# Why is the rule turning queries down? Counted, not guessed.
#
#   scripts/decline-reasons.sh                      # excluded min corpus
#   LIST=tmp/ce_runnable_sql.psv scripts/decline-reasons.sh
#
# A decline is cheap to read one at a time and nobody had read them in bulk.
# D108 left four excluded queries failing for a reason that is not the gate's
# -- "not a scan: PROJECTION", raised by the matcher before any cost is
# considered -- and whether that is four queries or four hundred decides
# whether it is worth anything.
set -uo pipefail
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
LIST=${LIST:-tmp/excluded_agg/agg_min.psv}
SAMPLE=${SAMPLE:-tmp/excluded_agg/min.csv}
CAP=${CAP:-60}

names=""
if [ -f "$SAMPLE" ]; then
    names=$(awk -F, 'NR>1 {print $1}' "$SAMPLE" | sort)
fi

while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    if [ -n "$names" ] && ! printf '%s\n' "$names" | grep -qxF "$name"; then
        continue
    fi
    q=$(printf '%s' "$q" | tr -d '\r')
    out=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "SET factorize_explain=true;" "EXPLAIN ${q};" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1)
    if printf '%s' "$out" | grep -qi Factorized; then
        echo "FIRED"
    else
        # The first clause of the reason is the category; the numbers after it
        # are per query and would make every line unique.
        printf '%s' "$out" | tr ',' '\n' | grep -o 'declined:.*' | head -1 |
            sed 's/declined: //; s/:.*//; s/ [0-9.].*//' || echo "(no reason printed)"
    fi
done <"$LIST" | sort | uniq -c | sort -rn
