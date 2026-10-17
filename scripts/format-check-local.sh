#!/usr/bin/env bash
# Check C++ formatting the way CI does, from a checkout where `.clang-format`
# is not a working symlink.
#
# Why this exists. The repo root `.clang-format` is a git symlink (mode 120000)
# pointing at duckdb/.clang-format. Git for Windows materialises it as an
# ordinary 23-byte text file containing the word "duckdb/.clang-format", so
# clang-format reads it, fails to parse it as YAML, and `make format-check`
# prints "Passed" after the error -- which is how forty consecutive CI runs
# went red on Format Check while the local check stayed green. `-style=file:<path>`
# would sidestep it but needs clang-format 14; CI pins 11.0.1, and a different
# version formats differently, so matching CI matters more than convenience.
#
# The workaround: mirror the sources into a scratch tree that has a *real*
# .clang-format at its root, format the copies there, and diff. Config lookup
# walks up from each source file, so the mirror only has to preserve relative
# paths, not absolute ones.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

CF="${CLANG_FORMAT:-}"
if [ -z "$CF" ]; then
    for cand in "$HOME/.local/bin/clang-format" clang-format-11 clang-format; do
        if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then CF="$cand"; break; fi
    done
fi
if [ -z "$CF" ]; then
    echo "no clang-format found; CI uses 11.0.1 (pip3 install --user clang_format==11.0.1)" >&2
    exit 2
fi
VERSION="$("$CF" --version)"
case "$VERSION" in
*" 11."*) ;;
*) echo "WARNING: $VERSION -- CI pins 11.0.1, and versions disagree about formatting." >&2 ;;
esac

# Resolve the config the symlink was supposed to point at.
CONFIG=".clang-format"
if [ ! -L .clang-format ]; then
    target="$(tr -d '\r\n' <.clang-format)"
    if [ -f "$target" ]; then
        CONFIG="$target"
        echo "note: .clang-format is an unresolved symlink; using $CONFIG"
    fi
fi

MIRROR="$(mktemp -d)"
trap 'rm -rf "$MIRROR"' EXIT
cp "$CONFIG" "$MIRROR/.clang-format"

# --others --exclude-standard as well as tracked files: `git ls-files`
# alone lists only what is already committed, so a brand-new file is
# invisible until after it is added -- which is exactly when it is most
# likely to be misformatted. src/core/int128.hpp and
# test/unit/test_int128.cpp were both skipped that way, the check
# reported the same 44 files before and after they existed, and CI's
# Format Check caught what this did not (D88).
mapfile -t FILES < <(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.hpp' 'src/*.h' 'test/*.cpp' 'test/*.hpp' | sort -u)
BAD=0
for f in "${FILES[@]}"; do
    mkdir -p "$MIRROR/$(dirname "$f")"
    cp "$REPO/$f" "$MIRROR/$f"
    (cd "$MIRROR" && "$CF" -i -style=file "$f")
    if ! diff -q "$REPO/$f" "$MIRROR/$f" >/dev/null; then
        echo "needs formatting: $f"
        if [ "${SHOW_DIFF:-0}" = 1 ]; then diff -u "$REPO/$f" "$MIRROR/$f" | head -40; fi
        BAD=$((BAD + 1))
    fi
done

if [ "$BAD" -gt 0 ]; then
    echo "$BAD of ${#FILES[@]} files are not formatted (run scripts/format-fix-local.sh)"
    exit 1
fi
echo "formatting OK (${#FILES[@]} files, $VERSION)"
