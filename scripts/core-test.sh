#!/usr/bin/env bash
# Build and run the standalone core tests. Deliberately does not involve DuckDB
# or CMake: src/core must stand on its own (plan section 4).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/factorize-core-test"
mkdir -p "$OUT"

CORE="src/core/ftree.cpp src/core/layout.cpp src/core/frep.cpp src/core/materialize.cpp src/core/join.cpp src/core/cost.cpp src/core/stats.cpp src/core/plan.cpp"
# C++14, not 17, and the tests enforce it rather than a comment asking nicely.
# The extension has to compile at C++14 (see CMakeLists.txt): C++17 makes
# DuckDB's static constexpr members implicitly inline and they collide at
# link time, while C++11 lacks make_unique and generic lambdas. Building the
# core standalone at 17 would let a C++17 feature in that only fails much
# later, inside a 13-minute DuckDB build.
FLAGS="-std=c++14 -g -Wall -Wextra -Wno-unused-parameter"
# Correctness runs use the sanitizers; the stability test is large, so it gets
# an optimized build too.
MODE="${1:-both}"

run() {
    echo "== $1 =="
    shift
    "$@"
}

if [ "$MODE" = "asan" ] || [ "$MODE" = "both" ]; then
    for t in test_ftree test_frep test_join test_cost test_plan test_enumerate test_outer; do
        ${CXX:-g++} $FLAGS -O1 -fsanitize=address,undefined -o "$OUT/$t.asan" "test/unit/$t.cpp" $CORE
        run "$t (asan+ubsan)" "$OUT/$t.asan"
    done
fi
if [ "$MODE" = "opt" ] || [ "$MODE" = "both" ]; then
    for t in test_ftree test_frep test_join test_cost test_plan test_enumerate test_outer; do
        ${CXX:-g++} $FLAGS -O2 -o "$OUT/$t" "test/unit/$t.cpp" $CORE
        run "$t (-O2)" "$OUT/$t"
    done
fi
