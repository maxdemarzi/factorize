# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(factorize
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# DuckDB 2.0 split building an extension from linking it: duckdb_extension_load
# only builds, and an extension reaches the shipped binaries only if it is also
# named here. The old DONT_LINK flag was removed and now raises FATAL_ERROR.
#
# Without this the build succeeds, prints "Extensions built but not linked:
# [factorize]" among the configure output, and produces a `duckdb` that does not
# know `factorize_mode` -- which is every measurement script in this repo
# silently comparing stock DuckDB against itself, and every SQL test skipping on
# `require factorize` rather than failing (D116).
duckdb_extension_statically_link(factorize)
