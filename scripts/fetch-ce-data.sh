#!/usr/bin/env bash
# Fetch the CE benchmark corpus and the cardinality oracle (DECISIONS.md D12).
#
# Neither ships with the FactDB artifact checkout. Both are downloads from TUM.
# The tarball is sha256-verified with the checksum the artifact publishes.
set -euo pipefail

FACTDB="${FACTDB:-$(cd "$(dirname "$0")/../../FactDB" && pwd)}"
CE_SHA256='2c3aaaa766930aee95e8c5d8e0b10c4526f960c585414f873a31070ee3049add'

echo "== CE benchmark data -> ${FACTDB}/bench/data/ce =="
mkdir -p "${FACTDB}/bench/data/ce"
cd "${FACTDB}/bench/data/ce"
if ! echo "${CE_SHA256}  cebench.tar.zst" | sha256sum --check --status 2>/dev/null; then
    curl -fSL --retry 3 --progress-bar -O https://db.in.tum.de/~birler/dbgen/cebench.tar.zst
fi
echo "${CE_SHA256}  cebench.tar.zst" | sha256sum --check

# The artifact's own setup.sh assumes zstd(1). This box has no root, so fall
# back to the `zstandard` Python wheel, which needs no system package.
if command -v zstd >/dev/null 2>&1; then
    tar --skip-old-files -xf cebench.tar.zst
else
    echo "zstd(1) absent; decompressing via python3 -m zstandard"
    python3 -c 'import zstandard' 2>/dev/null || pip3 install --user --quiet zstandard
    if [ ! -s cebench.tar ]; then
        python3 - <<'PY'
import zstandard, shutil
with open("cebench.tar.zst", "rb") as src, open("cebench.tar", "wb") as dst:
    zstandard.ZstdDecompressor().copy_stream(src, dst)
PY
    fi
    tar --skip-old-files -xf cebench.tar
fi

echo "CE data ready:"
du -sh .
printf '  csv files: '; ls *.csv 2>/dev/null | wc -l

echo
echo "== cardinality oracle (estimates.db) =="
# Section 0.6: running the harness with oracle cardinalities AND with DuckDB's
# own estimates separates plan quality from factorization. Nobody has published
# that split.
cd "${FACTDB}"
mkdir -p bench/data
if [ ! -s bench/data/estimates.db ]; then
    curl -fSL --retry 3 --progress-bar https://db.in.tum.de/~lehner/estimates.db -o bench/data/estimates.db
fi
ls -lh bench/data/estimates.db
