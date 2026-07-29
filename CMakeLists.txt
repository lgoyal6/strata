cmake_minimum_required(VERSION 3.24)
project(strata LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(STRATA_BUILD_TESTS "Build unit tests" ON)
option(STRATA_BUILD_TOOLS "Build crash harness and dump tool" ON)
option(STRATA_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(STRATA_BUILD_BENCH "Build YCSB benchmark (needs RocksDB for the baseline)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

add_library(strata STATIC
    src/util/crc32c.cc
    src/util/coding.cc
    src/util/arena.cc
    src/util/env.cc
    src/util/cache.cc
    src/util/status.cc
    src/wal/wal_writer.cc
    src/wal/wal_reader.cc
    src/table/block_builder.cc
    src/table/block.cc
    src/table/bloom.cc
    src/table/table_builder.cc
    src/table/table_reader.cc
    src/table/merging_iterator.cc
    src/db/dbformat.cc
    src/db/memtable.cc
    src/db/write_batch.cc
    src/db/table_cache.cc
    src/db/version.cc
    src/db/db_iter.cc
    src/db/db_impl.cc
)
target_include_directories(strata
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(strata PUBLIC cxx_std_20)
target_link_libraries(strata PRIVATE strata_warnings)
find_package(Threads REQUIRED)
target_link_libraries(strata PUBLIC Threads::Threads)

# Fuzz builds instrument the library itself with libFuzzer coverage
# counters (no main linked in) so fuzzing is coverage-guided through the
# real parsers, not just through the harness TU.
if(STRATA_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(strata PRIVATE -fsanitize=fuzzer-no-link)
endif()

if(STRATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
if(STRATA_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(STRATA_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
if(STRATA_BUILD_BENCH)
    add_subdirectory(bench)
endif()
