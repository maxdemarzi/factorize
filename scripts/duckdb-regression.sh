#!/usr/bin/env bash
# Run DuckDB's own tests with this extension loaded (plan §7.3, risk R9).
#
#   scripts/duckdb-regression.sh [suite ...]
#
# The optimizer rule is consulted on every plan in every query, which makes the
# whole engine the regression surface -- not just the queries the rule takes
# over. A rule that declines correctly still has to leave every other plan
# exactly as it found it, and the only convincing evidence for that is DuckDB's
# own suite passing with the extension in the process.
#
# Default mode is 'off', which is the configuration a user gets by installing
# the extension and doing nothing. That is the case worth defending hardest: an
# extension that changes results before anyone has opted into it would be the
# worst kind of bug.
set -euo pipefail
cd "$(dirname "$0")/.."

UNITTEST="${UNITTEST:-build/release/test/unittest}"
[ -x "$UNITTEST" ] || { echo "no unittest binary at $UNITTEST (make release)" >&2; exit 2; }

SUITES=("$@")
if [ ${#SUITES[@]} -eq 0 ]; then
    # The three that exercise what this extension touches: joins are the shape
    # it matches, aggregates are the operator it replaces, and the optimizer
    # tests are where a rule that rewrites plans wrongly shows up first.
    SUITES=("test/sql/join/*" "test/sql/aggregate/*" "test/optimizer/*")
fi

status=0
for suite in "${SUITES[@]}"; do
    echo "== $suite =="
    if ! "$UNITTEST" --test-dir duckdb "$suite" 2>&1 | tail -3; then
        status=1
    fi
done
exit $status
