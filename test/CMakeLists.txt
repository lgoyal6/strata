include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Build/link/parse canary with no framework dependency.
add_executable(strata_smoke smoke.cc)
target_link_libraries(strata_smoke PRIVATE strata strata_test_warnings)
target_include_directories(strata_smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME smoke COMMAND strata_smoke)

add_executable(strata_unit
    unit/coding_test.cc
    unit/wal_test.cc
    unit/memtable_test.cc
    unit/table_test.cc
    unit/manifest_test.cc
    unit/db_test.cc
    unit/db_model_test.cc
)
target_link_libraries(strata_unit PRIVATE strata GTest::gtest_main strata_test_warnings)
target_include_directories(strata_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(strata_unit)
