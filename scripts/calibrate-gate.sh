#!/usr/bin/env bash
# Phase 5's calibration: does factorize_mode='auto' ever make a query slower?
#
#   scripts/calibrate-gate.sh [timeout_seconds]
#
# For each CE query, times the same plain SQL under 'off' and under 'auto' in
# one session, and reports the ratio. Each mode is run twice and the second time
# taken, so the comparison is warm-cache against warm-cache rather than
# whichever mode happened to touch the pages first.
#
# The queries where 'off' does not finish are the point of the whole project, so
# they are recorded rather than dropped: a timeout is not a missing measurement,
# it is the largest possible win. The queries where 'off' finishes *quickly* are
# the ones that decide shippability, because that is where a rule that fires too
# eagerly does its damage.
#
# Build with `make release` first. A relassert build carries ASAN and UBSAN, and
# a timing taken under those is not a timing.
set -euo pipefail
cd "$(dirname "$0")/.."

DUCKDB="${DUCKDB:-build/release/duckdb}"
WORK="${TMPDIR:-/tmp}/factorize-duckdb"
DB="${DB:-$WORK/ce.db}"
TIMEOUT="${1:-60}"

[ -s tmp/ce_runnable.psv ] || { echo "run scripts/run-ce-extension.sh first" >&2; exit 2; }
[ -x "$DUCKDB" ] || { echo "no duckdb binary at $DUCKDB (make release)" >&2; exit 2; }

# Reports the seconds of the *second* "Run Time" line, which is the warm one.
timed() {
    local mode="$1" sql="$2"
    timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list \
        -c "SET factorize_mode='$mode';" -c ".timer on" -c "$sql" -c "$sql" 2>/dev/null |
        awk '/Run Time/ { match($0, /real [0-9.]+/); if (RSTART) print substr($0, RSTART+5, RLENGTH-5) }' |
        tail -1
}

# `force_s` times *this engine* on every query, whether or not the gate wanted
# it. Without that column the only measurements of our own cost come from the
# queries the gate already chose, which is the sample most likely to flatter it
# -- re-fitting on it once produced a model built from a single query.
echo "query,fired,off_s,auto_s,force_s,ratio,status"
while IFS='|' read -r name expected sql; do
    [ -n "$name" ] || continue

    plain=$(python3 - "$sql" <<'PY'
import re, sys
m = re.search(r"factorized_count\(\[(.*?)\], \[(.*?)\]\)", sys.argv[1])
tables = [t.strip().strip("'") for t in m.group(1).split(",")]
joins = [j.strip().strip("'") for j in m.group(2).split("', '")]
print(f"SELECT count(*) FROM {', '.join(tables)} WHERE {' AND '.join(joins)};")
PY
)

    fired=no
    if timeout "$TIMEOUT" "$DUCKDB" "$DB" -noheader -list \
        -c "SET factorize_mode='auto';" -c "EXPLAIN $plain" 2>/dev/null | grep -q FACTORIZED; then
        fired=yes
    fi

    off=$(timed off "$plain") || off=""
    auto=$(timed auto "$plain") || auto=""
    force=$(timed force "$plain") || force=""

    if [ -z "$off" ]; then
        # 'off' could not finish inside the timeout: the largest kind of win, and
        # not a ratio.
        printf '%s,%s,,%s,%s,,off-timeout\n' "$name" "$fired" "${auto:-}" "${force:-}"
        continue
    fi
    if [ -z "$auto" ]; then
        printf '%s,%s,%s,,%s,,auto-timeout\n' "$name" "$fired" "$off" "${force:-}"
        continue
    fi
    ratio=$(awk -v a="$off" -v b="$auto" 'BEGIN { if (b > 0) printf "%.3f", a / b; else print "" }')
    status=ok
    # A ratio under 1 means 'auto' was slower, but only two kinds of row can
    # mean it. If the gate declined, 'auto' *is* the stock plan and the two
    # numbers are the same query timed twice -- a ratio of 0.7 there measures
    # the clock, not the extension. And under a few tens of milliseconds the
    # clock is most of what is being measured either way. Flagging those as
    # regressions buries the ones that are real: the first calibration run
    # reported twelve, and five of them were this.
    if [ "$fired" = yes ] && awk -v a="$off" 'BEGIN { exit !(a > 0.05) }' &&
        awk -v r="$ratio" 'BEGIN { exit !(r < 0.8) }'; then
        status=REGRESSION
    fi
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$name" "$fired" "$off" "$auto" "${force:-}" "$ratio" "$status"
done < tmp/ce_runnable.psv
