add_executable(crash_test crash_test.cc)
target_link_libraries(crash_test PRIVATE strata strata_test_warnings)
target_include_directories(crash_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Compaction concurrency benchmark (no RocksDB needed, unlike bench/).
add_executable(compaction_bench compaction_bench.cc)
target_link_libraries(compaction_bench PRIVATE strata strata_test_warnings)
target_include_directories(compaction_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
