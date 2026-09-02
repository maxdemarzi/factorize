#!/usr/bin/env bash
# Phase 1.5 measurement: run CE query files through the standalone harness,
# factorized (both insert modes) and flat, on identical infrastructure.
#
#   scripts/run-ce.sh <output.csv> <shape> <dataset> [<dataset>...]
#
# e.g. scripts/run-ce.sh tmp/out.csv acyclic hetio watdiv yago
#
# Datasets are plain words, not a regex: wsl.exe strips quoting around anything
# containing shell metacharacters.
set -euo pipefail

# Re-exec from a private copy. Bash reads a script incrementally by byte offset,
# so editing this file mid-run makes it resume into the modified text -- which
# once truncated a completed result set by re-evaluating the output redirection.
# The repo root travels in an env var: after re-exec $0 points into /tmp, so a
# "$0"-relative cd would silently land somewhere else.
if [ "${CE_REEXEC:-}" != "1" ]; then
    CE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
    copy="$(mktemp "${TMPDIR:-/tmp}/run-ce.XXXXXX.sh")"
    cat "$0" > "$copy"
    CE_REEXEC=1 CE_ROOT="$CE_ROOT" exec bash "$copy" "$@"
fi
cd "${CE_ROOT:?}"

FACTDB="${FACTDB:-$(cd ../FactDB && pwd)}"
DATA="${FACTDB}/bench/data/ce"
QUERIES="${FACTDB}/bench/ce/queries"
OUT="${1:?output csv}"
SHAPE="${2:?acyclic|cyclic}"
shift 2
PATTERN=""
for d in "$@"; do
    case "$d" in --*) break ;; esac
    PATTERN="${PATTERN:+$PATTERN|}${d}_${SHAPE}"
    shift
done
[ -n "$PATTERN" ] || { echo "no datasets given" >&2; exit 2; }

# Each run builds its own binary: rebuilding to a fixed path while a previous
# run is executing it truncates the file under the running process.
BIN="$(mktemp "${TMPDIR:-/tmp}/factorize-standalone.XXXXXX")"
trap 'rm -f "$BIN"' EXIT
g++ -std=c++17 -O2 -g -o "$BIN" src/bench/standalone.cpp \
    src/core/ftree.cpp src/core/layout.cpp src/core/frep.cpp \
    src/core/materialize.cpp src/core/join.cpp src/core/cost.cpp src/core/stats.cpp

args=()
count=0
for f in $(ls "$QUERIES" | grep -E "$PATTERN"); do
    args+=(--query "$QUERIES/$f")
    count=$((count + 1))
done
echo "running $count query files matching /$PATTERN/" >&2

mkdir -p "$(dirname "$OUT")"
# Write to a scratch path and move into place only on success, so a failed or
# re-entered run cannot destroy a previous good result.
TMP_OUT="${OUT}.partial"
"$BIN" --data "$DATA" "${args[@]}" \
    --max-flat-rows "${MAX_FLAT_ROWS:-20000000}" \
    --max-frep-bytes "${MAX_FREP_BYTES:-4000000000}" \
    --max-cache-bytes "${MAX_CACHE_BYTES:-2000000000}" \
    "$@" > "$TMP_OUT"
mv "$TMP_OUT" "$OUT"
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) queries)" >&2
