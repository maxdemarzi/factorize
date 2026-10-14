#!/usr/bin/env bash
# What the fan-out floor is worth when actually run, rather than modelled.
#
#   scripts/measure-fanout-floor.sh                  # tmp/fanout_floor.csv
#   LIST=tmp/agg_min.psv OUT=tmp/ff_min.csv scripts/measure-fanout-floor.sh
#
# D84 priced the floor by assuming an abandoned query costs exactly what the
# stock plan costs. It does not: it costs the stock plan *plus* whatever the
# factorized attempt already spent before giving up.
#
# This measured it, and the floor did not survive. Over the 119 runnable
# queries, restricted to the 36 the gate fires on, auto is 2.126s and auto
# with the floor is 2.171s -- the modelled 4 regressions cut were entirely the
# modelling artifact, and D84's reluctance to ship a knob on modelled numbers
# was right. The gate wiring was reverted in the same commit that recorded
# this (D85); `factorize_min_fanout` and `factorize_fanout_max_compression`
# exist only in that commit's parent, so re-running this needs them restored.
# The script is kept because the result is worth being able to reproduce, not
# because the floor is worth another try without a new idea behind it.
#
# Three timings per query: stock, auto as it ships, and auto with the floor
# on. One discarded warm-up then two timed runs, the faster kept -- the
# methodology every other measurement here uses.
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/clean-spill.sh"
CleanSpill
trap CleanSpill EXIT

D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-120}
OUT=${OUT:-tmp/fanout_floor.csv}
LIST=${LIST:-tmp/ce_runnable_sql.psv}
FLOOR=${FLOOR:-1.0}
CEIL=${CEIL:-50.0}

echo "query,off_s,auto_s,floor_s,fired" > "$OUT"
# The last two Run Times, not lines 3 and 4. `.timer on` prints one for every
# statement including the SETs, so a positional window silently reads a SET's
# timing whenever the number of SETs changes -- this run has three where the
# others have one, and the first version of this script reported a 620x
# speedup made entirely of `SET factorize_min_fanout` taking 0.000s.
best() { grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | tail -2 | sort -g | head -1; }

while IFS='|' read -r name expected q; do
    [ -n "$name" ] || continue
    q=$(echo "$q" | tr -d '\r')
    off=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}off${Q};" "$q;" "$q;" "$q;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    auto=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}auto${Q};" "$q;" "$q;" "$q;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    floor=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}auto${Q};" \
        "SET factorize_min_fanout=${FLOOR};" "SET factorize_fanout_max_compression=${CEIL};" \
        "$q;" "$q;" "$q;" | timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | best)
    f=$(printf '%s\n' "SET factorize_mode=${Q}auto${Q};" "EXPLAIN $q;" |
        timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1 | grep -c FACTORIZED || true)
    echo "${name},${off:-to},${auto:-to},${floor:-to},${f}" >> "$OUT"
    echo "  $name off=${off:-to} auto=${auto:-to} floor=${floor:-to}" >&2
done < "$LIST"

python3 - "$OUT" <<'PYEOF'
import csv, sys
def num(v, cap=120.0):
    return cap if v in ("to", "", None) else float(v)
rows = list(csv.DictReader(open(sys.argv[1])))
off = sum(num(r["off_s"]) for r in rows)
auto = sum(num(r["auto_s"]) for r in rows)
floor = sum(num(r["floor_s"]) for r in rows)
fires = [r for r in rows if r["fired"] != "0"]
regr_auto = sum(1 for r in rows if num(r["auto_s"]) > num(r["off_s"]) * 1.05)
regr_floor = sum(1 for r in rows if num(r["floor_s"]) > num(r["off_s"]) * 1.05)
helped = [r for r in rows if num(r["floor_s"]) < num(r["auto_s"]) * 0.95]
hurt = [r for r in rows if num(r["floor_s"]) > num(r["auto_s"]) * 1.05]
print(f"\n{len(rows)} queries, {len(fires)} fire")
print(f"  stock {off:.2f}s | auto {auto:.2f}s ({off/auto:.2f}x) | floor {floor:.2f}s ({off/floor:.2f}x)")
print(f"  regressions against stock: auto {regr_auto}, floor {regr_floor}")
print(f"  floor helped {len(helped)}, hurt {len(hurt)}")
for r in sorted(hurt, key=lambda r: num(r["auto_s"]) - num(r["floor_s"]))[:5]:
    print(f"    hurt: {r['query']} auto {r['auto_s']} -> floor {r['floor_s']}")
PYEOF
echo "=== -> $OUT"
