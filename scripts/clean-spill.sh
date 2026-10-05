#!/usr/bin/env bash
# Remove DuckDB spill files the measurement scripts leave behind.
#
#   scripts/clean-spill.sh            # once, by hand
#   . scripts/clean-spill.sh          # sourced; defines CleanSpill
#
# Every script here caps its queries with `timeout`, which is the whole point --
# a query that does not finish is the measurement. But a killed process never
# runs DuckDB's own cleanup, so its `duckdb_temp_storage_*.tmp` files stay in
# the database's temp directory. Roughly half of an excluded-corpus run ends
# that way, and one day of measuring left 23GB of them (18 files, the largest
# 4GB).
#
# Deleting is safe only when nothing is running, because the temp directory is
# shared: two measurement scripts at once, and this would pull the live one's
# spill out from under it. So it refuses while any duckdb process exists rather
# than guessing which files are whose.
CleanSpill() {
    local db=${DB:-/tmp/factorize-duckdb/ce.db}
    local dir="${db}.tmp"
    [ -d "$dir" ] || return 0
    if pgrep -x duckdb >/dev/null 2>&1; then
        echo "clean-spill: duckdb is running, leaving $dir alone" >&2
        return 0
    fi
    local n
    n=$(find "$dir" -maxdepth 1 -name 'duckdb_temp_storage_*.tmp' | wc -l)
    [ "$n" -gt 0 ] || return 0
    local size
    size=$(du -sh "$dir" 2>/dev/null | cut -f1)
    find "$dir" -maxdepth 1 -name 'duckdb_temp_storage_*.tmp' -delete
    echo "clean-spill: removed $n spill files ($size) from $dir" >&2
}

# Run it when executed rather than sourced.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    CleanSpill
fi
