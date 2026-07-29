# Two interface targets: strict warnings for the library, relaxed for tests
# and tools (GoogleTest headers trip the strict set).
add_library(strata_warnings INTERFACE)
target_compile_options(strata_warnings INTERFACE
    -Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
    -Wnull-dereference -Wdouble-promotion)

add_library(strata_test_warnings INTERFACE)
target_compile_options(strata_test_warnings INTERFACE -Wall -Wextra)
