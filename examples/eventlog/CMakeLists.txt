# Standalone consumer: this directory is NOT part of the strata build.
# It is configured on its own against an installed strata, which is the
# thing worth testing - an in-tree add_subdirectory would prove nothing
# about the installed package.
#
#   cmake --install <strata-build> --prefix /tmp/strata-prefix
#   cmake -S examples/eventlog -B /tmp/eventlog -DCMAKE_PREFIX_PATH=/tmp/strata-prefix
#   cmake --build /tmp/eventlog && /tmp/eventlog/eventlog /tmp/eventlog-db
cmake_minimum_required(VERSION 3.20)
project(strata_eventlog LANGUAGES CXX)

find_package(strata 0.1 REQUIRED)

add_executable(eventlog main.cc)
target_link_libraries(eventlog PRIVATE strata::strata)
target_compile_features(eventlog PRIVATE cxx_std_20)
