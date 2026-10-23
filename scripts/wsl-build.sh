#!/usr/bin/env bash
# Build the factorize extension from WSL against the pinned DuckDB (see DECISIONS.md D4, D7).
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_TYPE="${1:-relassert}"

# The same version string .github/workflows/MainDistributionPipeline.yml
# passes as `duckdb_version`. Without it DuckDB stamps the engine's git hash
# into the loadable extension's metadata instead, and the released CLI --
# which calls itself v2.0.0 -- refuses to load it: "built specifically for
# DuckDB version 'd54cbbe3df'". The statically linked binary this script is
# usually used for does not care, which is exactly why it went unnoticed
# until something finally loaded the artifact (D128).
export DUCKDB_VERSION="${DUCKDB_VERSION:-v2.0.0}"
export GEN=ninja
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
export EXT_FLAGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
# Nothing beyond core DuckDB is needed to exercise the optimizer rule.
export BUILD_EXTENSION_TEST_DEPS=none
export CORE_EXTENSIONS=""

echo "== factorize: building '${BUILD_TYPE}' =="
cmake --version | head -1
"${CXX:-g++}" --version | head -1
echo

time make "${BUILD_TYPE}" -j"$(nproc)"
