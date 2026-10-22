cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME factorize)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# This is a no-op for the factorize targets specifically -- see the comment
# below at the CXX_STANDARD override for why. Kept in step with it so the two
# cannot contradict each other even though this line does not take effect.
set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# src/core/ is deliberately free of DuckDB headers (plan §4): it is compiled and
# benchmarked standalone in Phase 1 so the go/no-go measurement stays honest.
set(CORE_SOURCES
    src/core/ftree.cpp
    src/core/layout.cpp
    src/core/frep.cpp
    src/core/materialize.cpp
    src/core/join.cpp
    src/core/cost.cpp
    src/core/stats.cpp
    src/core/plan.cpp)

set(DUCKDB_GLUE_SOURCES
    src/duckdb/logical_factorized.cpp
    src/duckdb/physical_factorized.cpp
    src/duckdb/optimizer_rule.cpp
    src/duckdb/storage_source.cpp
    src/duckdb/table_function.cpp)

set(EXTENSION_SOURCES
    src/factorize_extension.cpp
    ${DUCKDB_GLUE_SOURCES}
    ${CORE_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The extension builds at C++17, which DuckDB 2.0 requires: its CMakeLists
# refuses anything lower outright.
#
# This used to be 14, and the reason it could not be 17 has been removed rather
# than worked around. DuckDB 1.x compiled at C++11, so `static constexpr`
# members were not implicitly inline on its side; a C++17 translation unit
# including its headers emitted its own definitions of things like
# duckdb::LogicalType::BIGINT and they collided at link time with the
# out-of-line ones DuckDB's build emitted. Both sides are C++17 now, so both
# treat those members the same way and there is nothing to collide.
#
# src/core/ stays conservative anyway -- see the Byte alias in core/arena.hpp,
# which stands in for std::byte -- because it is built standalone by
# scripts/core-test.sh and that is the property the alias protects, not the
# link-time one that has gone away.
foreach(factorize_target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  set_target_properties(
    ${factorize_target}
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
