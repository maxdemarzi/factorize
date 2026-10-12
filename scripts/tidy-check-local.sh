#!/usr/bin/env bash
# Run CI's clang-tidy checks over this extension's own sources.
#
# `make tidy-check` (extension-ci-tools) configures a second, non-unity DuckDB
# build tree just to produce a compile database, which is a 20-minute detour on
# this machine. The release build already emits build/release/compile_commands.json
# and our fourteen files are compiled individually there, so tidy can read that
# instead. Same checks, same config, minutes instead of an hour.
#
# The config has to be passed explicitly: the root .clang-format/.clang-tidy are
# git symlinks that Git for Windows materialises as text (see
# scripts/format-check-local.sh), so letting clang-tidy discover the config by
# walking up would feed it a one-line path instead of YAML.
#
# CI uses ubuntu-24.04's clang-tidy (18). Match it:
#     pip3 install --user clang-tidy==18.1.1
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

BUILD="${TIDY_BUILD_DIR:-build/release}"
if [ ! -f "$BUILD/compile_commands.json" ]; then
    echo "no $BUILD/compile_commands.json -- build first (scripts/wsl-build.sh release)" >&2
    exit 2
fi

CT="${CLANG_TIDY:-}"
if [ -z "$CT" ]; then
    for cand in "$HOME/.local/bin/clang-tidy" clang-tidy-18 clang-tidy; do
        if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then CT="$cand"; break; fi
    done
fi
if [ -z "$CT" ]; then
    echo "no clang-tidy found; CI uses 18 (pip3 install --user clang-tidy==18.1.1)" >&2
    exit 2
fi

CONFIG="duckdb/.clang-tidy"
LOG="${TMPDIR:-/tmp}/factorize-tidy.log"

# Only files the compile database knows about. CI's run-clang-tidy.py filters
# the database the same way, so a source the extension CMake does not build --
# src/bench/standalone.cpp, say -- is never tidied there either. Running it
# here anyway would make clang-tidy guess the flags and report failures CI
# cannot see, so list those files instead of inventing diagnostics for them.
mapfile -t FILES < <(python3 - "$BUILD/compile_commands.json" "$REPO" <<'PY'
import json, os, subprocess, sys
db, repo = sys.argv[1], sys.argv[2]
known = {os.path.realpath(e["file"]) for e in json.load(open(db))}
tracked = subprocess.check_output(["git", "ls-files", "src/*.cpp"], text=True).split()
inside = [t for t in tracked if os.path.realpath(os.path.join(repo, t)) in known]
outside = [t for t in tracked if os.path.realpath(os.path.join(repo, t)) not in known]
for f in inside:
    print(f)
if outside:
    print("not in the compile database, so CI does not tidy them either: "
          + ", ".join(outside), file=sys.stderr)
PY
)

"$CT" --version | head -3
printf '%s\n' "${FILES[@]}" |
    xargs -P "$(nproc)" -I{} "$CT" -p "$BUILD" --config-file="$CONFIG" \
        --header-filter="$REPO/src/.*" --quiet {} >"$LOG" 2>&1 || true

# "N warnings generated." is clang-tidy counting diagnostics it then filtered
# out by header-filter -- almost all of them from DuckDB's own headers. Only
# lines that actually name a file and a check are failures.
if grep -E "^.*\.(cpp|hpp|h):[0-9]+:[0-9]+: (warning|error):" "$LOG"; then
    echo "tidy found problems (full log: $LOG)"
    exit 1
fi
echo "tidy clean (${#FILES[@]} files, log: $LOG)"
