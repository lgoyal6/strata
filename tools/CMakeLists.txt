add_executable(crash_test crash_test.cc)
target_link_libraries(crash_test PRIVATE strata strata_test_warnings)
target_include_directories(crash_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
