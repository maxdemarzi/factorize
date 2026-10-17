#!/usr/bin/env bash
# Apply CI's formatting in place. Same symlink workaround as
# scripts/format-check-local.sh -- read the comment at the top of that file.
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

CONFIG=".clang-format"
if [ ! -L .clang-format ]; then
    target="$(tr -d '\r\n' <.clang-format)"
    [ -f "$target" ] && CONFIG="$target"
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
CHANGED=0
for f in "${FILES[@]}"; do
    mkdir -p "$MIRROR/$(dirname "$f")"
    cp "$REPO/$f" "$MIRROR/$f"
    (cd "$MIRROR" && "$CF" -i -style=file "$f")
    if ! diff -q "$REPO/$f" "$MIRROR/$f" >/dev/null; then
        cp "$MIRROR/$f" "$REPO/$f"
        echo "formatted: $f"
        CHANGED=$((CHANGED + 1))
    fi
done
echo "$CHANGED of ${#FILES[@]} files rewritten"
