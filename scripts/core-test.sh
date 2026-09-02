#!/usr/bin/env bash
# Build and run the standalone core tests. Deliberately does not involve DuckDB
# or CMake: src/core must stand on its own (plan section 4).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/factorize-core-test"
mkdir -p "$OUT"

CORE="src/core/ftree.cpp src/core/layout.cpp src/core/frep.cpp src/core/materialize.cpp src/core/join.cpp src/core/cost.cpp src/core/stats.cpp src/core/plan.cpp"
FLAGS="-std=c++17 -g -Wall -Wextra -Wno-unused-parameter"
# Correctness runs use the sanitizers; the stability test is large, so it gets
# an optimized build too.
MODE="${1:-both}"

run() {
    echo "== $1 =="
    shift
    "$@"
}

if [ "$MODE" = "asan" ] || [ "$MODE" = "both" ]; then
    for t in test_ftree test_frep test_join test_cost; do
        ${CXX:-g++} $FLAGS -O1 -fsanitize=address,undefined -o "$OUT/$t.asan" "test/unit/$t.cpp" $CORE
        run "$t (asan+ubsan)" "$OUT/$t.asan"
    done
fi
if [ "$MODE" = "opt" ] || [ "$MODE" = "both" ]; then
    for t in test_ftree test_frep test_join test_cost; do
        ${CXX:-g++} $FLAGS -O2 -o "$OUT/$t" "test/unit/$t.cpp" $CORE
        run "$t (-O2)" "$OUT/$t"
    done
fi
