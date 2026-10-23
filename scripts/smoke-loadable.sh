#!/usr/bin/env bash
# Load the extension the way a user does, into a DuckDB that was not built
# with it.
#
#   STOCK=/path/to/stock/duckdb scripts/smoke-loadable.sh
#
# Everything else in this repository tests `build/release/duckdb`, which has
# factorize statically linked in. That is not what gets published. The
# published artifact is `factorize.duckdb_extension`, and until this script
# existed nothing had ever loaded one -- which is the same blind spot D116
# found the hard way, where the extension built green, produced artifacts,
# and was never linked into the binary that shipped.
#
# The stock binary has to be the *released* CLI for the version in
# `.github/workflows/MainDistributionPipeline.yml`, because that is the
# version string the published build records and the one DuckDB checks:
#
#   gh release download v2.0.0 --repo duckdb/duckdb \
#     --pattern 'duckdb-cli-linux-amd64.tar.gz' --dir /tmp/duckdb-stock
#
# Build the extension with DUCKDB_VERSION set to the same string, or the
# artifact records the engine's git hash instead and the released CLI refuses
# it -- which is what a local `make release` produces by default (D128).
set -euo pipefail
cd "$(dirname "$0")/.."

STOCK=${STOCK:-/tmp/duckdb-stock/duckdb}
[ -x "$STOCK" ] || { echo "no stock duckdb at $STOCK (see the header)" >&2; exit 2; }

# Newest, not first found: the repository directory keeps one subdirectory
# per version string the tree has been built under, and a stale one from a
# build without DUCKDB_VERSION set is exactly the artifact that will not load.
EXT=${EXT:-$(find build/release/repository -name 'factorize.duckdb_extension' -printf '%T@ %p
' |
             sort -rn | head -1 | cut -d' ' -f2-)}
[ -n "$EXT" ] && [ -f "$EXT" ] || { echo "no factorize.duckdb_extension under build/release/repository" >&2; exit 2; }
EXT=$(cd "$(dirname "$EXT")" && pwd)/$(basename "$EXT")

echo ":: stock  $("$STOCK" -noheader -list -c 'pragma version')"
echo ":: ext    $EXT"

fail=0
check() {  # $1 = what, $2 = got, $3 = want
    if [ "$2" = "$3" ]; then
        printf '  ok   %-44s %s\n' "$1" "$2"
    else
        printf '  FAIL %-44s got [%s] want [%s]\n' "$1" "$2" "$3"
        fail=1
    fi
}

# A dataset of its own, so the check needs no corpus and runs anywhere. Two
# relations joined many-to-many: 200 x 200 keys meeting on a shared column is
# the shape the engine exists for, and small enough to answer instantly.
setup="LOAD '$EXT';
CREATE TABLE r AS SELECT i % 20 AS k, i AS v FROM range(200) t(i);
CREATE TABLE s AS SELECT i % 20 AS k, i AS v FROM range(200) t(i);
CREATE TABLE u AS SELECT i % 20 AS k, i AS v FROM range(200) t(i);"

ask() {  # $1 = mode, $2 = query
    "$STOCK" -unsigned -noheader -list -c "$setup
SET factorize_mode='$1';
$2" 2>&1 | tail -1
}

check "the extension loads at all" "$(ask off "SELECT 'yes';")" "yes"
check "the setting it registers exists" \
      "$(ask off "SELECT current_setting('factorize_mode');")" "off"

# Every aggregate the engine answers, forced through it, against the same
# query with the engine switched off. `force` rather than `auto`: the gate
# would decline a dataset this small, and then this would be comparing stock
# DuckDB with itself -- the mistake D123 and D126 both record.
for agg in "count(*)" "sum(r.v)" "min(r.v)" "max(r.v)" "avg(r.v)" "count(r.v)"; do
    q="SELECT $agg FROM r, s, u WHERE r.k = s.k AND s.k = u.k;"
    check "$agg agrees with the engine off" "$(ask force "$q")" "$(ask off "$q")"
done

q="SELECT r.k, count(*), sum(r.v) FROM r, s, u WHERE r.k = s.k AND s.k = u.k GROUP BY r.k ORDER BY r.k;"
check "GROUP BY agrees with the engine off" \
      "$(ask force "$q" | tail -1)" "$(ask off "$q" | tail -1)"

# And that it really went through the engine rather than quietly declining.
plan=$("$STOCK" -unsigned -noheader -list -c "$setup
SET factorize_mode='force';
EXPLAIN SELECT count(*) FROM r, s, u WHERE r.k = s.k AND s.k = u.k;" 2>&1 | grep -ci factorized || true)
check "the plan actually says FACTORIZED" "$([ "$plan" -gt 0 ] && echo yes || echo no)" "yes"

[ "$fail" -eq 0 ] && echo ":: loadable extension OK" || { echo ":: loadable extension FAILED"; exit 1; }
