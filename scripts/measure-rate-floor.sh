#!/usr/bin/env bash
# What `factorize_min_rate` does to the queries whose outcome is already known.
#
#   scripts/measure-rate-floor.sh            # writes tmp/rate_floor.csv
#   FLOOR=70000 CAP=300 scripts/...
#
# The 25 queries below are the measured-outcome table D47 asked for and D51/D52
# were judged against: SHOULD-FIRE means the factorized path answers something
# the stock plan does not, or answers it much faster; should-not means the stock
# plan is better and firing costs time. They are the only queries on this corpus
# whose right answer is known rather than predicted.
#
# The floor abandons at the last materialized join when the cumulative rate is
# below it, and abandoning lands on the §7.5 fallback -- so a query that
# abandons is timed end to end, our part plus the stock plan's.
#
# What would make the floor shippable: every SHOULD-FIRE still answered, and
# some should-not returned to roughly the stock time. What would sink it: a
# single SHOULD-FIRE abandoned, since those are the queries the engine exists
# for.
#
# No set -e: hitting the cap is the result for some of these.
cd "$(dirname "$0")/.."
D=${BIN:-build/release/duckdb}; DB=${DB:-/tmp/factorize-duckdb/ce.db}; Q="'"
CAP=${CAP:-300}
FLOOR=${FLOOR:-70000}
OUT=${OUT:-tmp/rate_floor.csv}

SHOULD_FIRE="hetio_acyclic_203_09 hetio_acyclic_205_03 hetio_acyclic_205_09 hetio_acyclic_205_11
hetio_acyclic_205_15 hetio_acyclic_210_05 hetio_acyclic_210_09 hetio_acyclic_211_07
hetio_acyclic_211_10 hetio_acyclic_211_14 hetio_acyclic_216_01 hetio_acyclic_216_14
hetio_acyclic_222_07 hetio_acyclic_225_02 watdiv_acyclic_210_06"
SHOULD_NOT="hetio_acyclic_204_01 hetio_acyclic_204_08 watdiv_acyclic_205_19 watdiv_acyclic_217_05
watdiv_acyclic_217_10 watdiv_acyclic_217_15 watdiv_acyclic_218_15 yago_acyclic_Chain_12_06
yago_acyclic_Chain_12_63 yago_acyclic_Chain_9_71"

echo "query,want,floor_off_s,floor_on_s,abandoned" > "$OUT"
run() {  # $1 = sql, $2 = floor
  local out status secs fell
  # threads=1, so the rate being judged is the whole query's rather than one
  # bucket's. The floor is compared against numbers measured that way, and a
  # slice of a query delivers a different rate from the query.
  out=$(printf '%s\n' ".timer on" "SET factorize_mode=${Q}force${Q};" "SET threads=1;" \
        "SET factorize_min_rate=$2;" "$1;" | timeout "$CAP" "$D" -readonly "$DB" -noheader -list 2>&1)
  status=$?
  fell=0
  echo "$out" | grep -q "fell back" && fell=1
  if [ "$status" -eq 124 ]; then
    # Hit the cap. Reporting the last Run Time here would report a SET
    # statement's, which is how a timeout once got recorded as a 6ms answer.
    echo "to|$fell"
    return
  fi
  secs=$(echo "$out" | grep "Run Time" | sed 's/.*real //' | awk '{print $1}' | tail -1)
  echo "${secs:-to}|$fell"
}

for want in SHOULD-FIRE should-not; do
  if [ "$want" = "SHOULD-FIRE" ]; then names="$SHOULD_FIRE"; else names="$SHOULD_NOT"; fi
  for name in $names; do
    q=$(grep -h "^${name}|" tmp/ce_runnable_sql.psv tmp/excluded_all.psv 2>/dev/null |
        head -1 | cut -d'|' -f3- | tr -d '\r')
    [ -n "$q" ] || { echo "no sql for $name"; continue; }
    off=$(run "$q" 0); on=$(run "$q" "$FLOOR")
    echo "${name},${want},${off%%|*},${on%%|*},${on##*|}" >> "$OUT"
    printf '%-26s %-11s floor off %-8s floor on %-8s %s\n' "$name" "$want" "${off%%|*}" "${on%%|*}" \
        "$([ "${on##*|}" = "1" ] && echo abandoned)"
  done
done
echo "=== written to $OUT (floor $FLOOR tuples/ms, cap ${CAP}s)"
