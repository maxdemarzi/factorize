cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME factorize)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# This is a no-op for the factorize targets specifically -- see the comment
# below at the CXX_STANDARD 14 override for why, and why 14 is correct. Left
# at 14 (not 17) so this line does not contradict that override even though it
# currently does not take effect.
set(CMAKE_CXX_STANDARD "14" CACHE STRING "C++ standard to enforce")
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

# The extension builds at C++14, which is the only standard that works here.
#
# DuckDB compiles at C++11, and C++17 is not an option: it makes `static
# constexpr` members implicitly inline, so a translation unit including DuckDB
# headers emits its own definitions of things like duckdb::LogicalType::BIGINT
# and they collide at link time with the out-of-line ones DuckDB's own build
# emits. C++11 is not an option either: src/core/ needs std::make_unique and
# generic lambdas, both C++14. C++14 has neither problem.
#
# src/core/ is kept free of C++17 (see the Byte alias in core/arena.hpp, which
# exists in place of std::byte for exactly this reason).
foreach(factorize_target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    set_target_properties(${factorize_target} PROPERTIES
        CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endforeach()


install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
