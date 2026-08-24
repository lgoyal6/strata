cmake_minimum_required(VERSION 3.24)
project(strata LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# When strata is pulled in by another project (FetchContent, add_subdirectory)
# it should build the library and nothing else: a consumer has no use for our
# crash harness, and building it would make their configure step fail on a
# missing RocksDB or a non-clang compiler.
if(PROJECT_IS_TOP_LEVEL)
    set(STRATA_DEFAULT_EXTRAS ON)
else()
    set(STRATA_DEFAULT_EXTRAS OFF)
endif()

option(STRATA_BUILD_TESTS "Build unit tests" ${STRATA_DEFAULT_EXTRAS})
option(STRATA_BUILD_TOOLS "Build crash harness and dump tool" ${STRATA_DEFAULT_EXTRAS})
option(STRATA_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(STRATA_BUILD_BENCH "Build YCSB benchmark (needs RocksDB for the baseline)" OFF)
option(STRATA_INSTALL "Generate install and package-config rules" ${STRATA_DEFAULT_EXTRAS})

include(GNUInstallDirs)
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
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# Consumers link strata::strata, in-tree and installed alike, so a project can
# switch between FetchContent and find_package without touching its code.
add_library(strata::strata ALIAS strata)
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

# --------------------------------------------------------------- install ---
#
# Two supported ways to consume strata, both landing on the same
# strata::strata target:
#
#   FetchContent_Declare(strata GIT_REPOSITORY https://github.com/lgoyal6/strata.git
#                               GIT_TAG v0.1.0)
#   FetchContent_MakeAvailable(strata)
#   target_link_libraries(app PRIVATE strata::strata)
#
#   cmake --install build --prefix /usr/local   # then, from anywhere:
#   find_package(strata 0.1 REQUIRED)
#   target_link_libraries(app PRIVATE strata::strata)
if(STRATA_INSTALL)
    include(CMakePackageConfigHelpers)

    # strata_warnings is a build-time-only interface target. It must be
    # installed alongside, or the exported target references a target the
    # consumer's project has never heard of and find_package fails.
    install(TARGETS strata strata_warnings
            EXPORT strataTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/strata DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT strataTargets
            FILE strataTargets.cmake
            NAMESPACE strata::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/strata)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/strataConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/strataConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/strataConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/strata)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/strataConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/strataConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/strata)
endif()
