# RocksDB baseline via Homebrew/system install; the bench builds without it
# (strata engine only) so CI needn't install RocksDB.
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h HINTS /opt/homebrew/opt/rocksdb/include /usr/local/include)
find_library(ROCKSDB_LIBRARY rocksdb HINTS /opt/homebrew/opt/rocksdb/lib /usr/local/lib)

add_executable(bench
    bench_main.cc
    engine_strata.cc
    engine_rocksdb.cc
)
target_link_libraries(bench PRIVATE strata strata_test_warnings)
target_include_directories(bench PRIVATE ${CMAKE_SOURCE_DIR})

if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
    message(STATUS "bench: RocksDB baseline enabled (${ROCKSDB_LIBRARY})")
    target_compile_definitions(bench PRIVATE STRATA_HAVE_ROCKSDB)
    target_include_directories(bench PRIVATE ${ROCKSDB_INCLUDE_DIR})
    target_link_libraries(bench PRIVATE ${ROCKSDB_LIBRARY})
else()
    message(WARNING "bench: RocksDB not found; building strata engine only")
endif()
