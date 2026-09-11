#!/usr/bin/env bash
# Predicted vs measured records, per step, paired by relation.
#
#   LIST=tmp/queries.txt OUT=tmp/rec_steps.csv scripts/compare-records.sh
#
# The gate prints what it expects standing after each step (factorize_explain);
# the operator prints what it actually built. Pairing them by relation is what
# turned "the record estimate is 1,000x over" into "1.0x at the first step,
# 5.8x by the last, and here is the step where it departs" (D49).
#
# threads=1 so the measured numbers are the whole query rather than one slice.
# Records are deterministic, so this is not a timing and needs no warm-up.
# Query names are looked up in tmp/ce_runnable_sql.psv and tmp/excluded_all.psv.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=/tmp/factorize-duckdb/ce.db; Q="'"
OUT=${OUT:-tmp/rec_steps.csv}
echo "query,step,relation,predicted,measured" > "$OUT"
while read -r name; do
  [ -n "$name" ] || continue
  q=$(grep "^${name}|" tmp/ce_runnable_sql.psv tmp/excluded_all.psv 2>/dev/null | head -1 | cut -d'|' -f3- | tr -d '\r')
  [ -n "$q" ] || { echo "no sql for $name"; continue; }
  pred=$(timeout 120 "$D" -readonly "$DB" -noheader -list -c "SET factorize_mode=${Q}auto${Q};" \
         -c "SET factorize_explain=true;" -c "EXPLAIN $q;" 2>&1 | grep -o 'gate predicted:.*' | head -1)
  meas=$(timeout 300 "$D" -readonly "$DB" -noheader -list -c "SET threads=1;" \
         -c "SET factorize_mode=${Q}force${Q};" -c "SET factorize_explain=true;" -c "$q;" 2>&1 | grep -o 'slice 0.*')
  [ -n "$pred" ] && [ -n "$meas" ] || { echo "skipped $name"; continue; }
  python3 - "$name" "$pred" "$meas" "$OUT" <<'PY'
import re, sys
name, pred, meas, out = sys.argv[1:5]
predicted = {int(r): float(v) for r, v in re.findall(r"\[(\d+): ([0-9.e+-]+) recs\]", pred)}
measured = {int(r): float(v) for r, v in re.findall(r"\[(\d+): (\d+) recs", meas)}
order = [int(r) for r, _ in re.findall(r"\[(\d+): ([0-9.e+-]+) recs\]", pred)]
with open(out, "a") as handle:
    for step, relation in enumerate(order):
        if relation in measured:
            handle.write(f"{name},{step},{relation},{predicted[relation]:.6g},{measured[relation]:.0f}\n")
PY
  printf '%-28s %s steps paired\n' "$name" "$(grep -c "^${name}," "$OUT")"
done < "${LIST:-/tmp/rec_list.txt}"
echo "=== $(( $(wc -l < "$OUT") - 1 )) step rows in $OUT"
