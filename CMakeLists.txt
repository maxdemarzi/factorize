cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME factorize)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# src/core/ is deliberately free of DuckDB headers (plan §4): it is compiled and
# benchmarked standalone in Phase 1 so the go/no-go measurement stays honest.
set(CORE_SOURCES)

set(DUCKDB_GLUE_SOURCES
    src/duckdb/logical_factorized.cpp
    src/duckdb/physical_factorized.cpp
    src/duckdb/optimizer_rule.cpp)

set(EXTENSION_SOURCES
    src/factorize_extension.cpp
    ${DUCKDB_GLUE_SOURCES}
    ${CORE_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
