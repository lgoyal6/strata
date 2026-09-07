cmake_minimum_required(VERSION 3.24)
project(strata_compat_consumer LANGUAGES CXX)

find_package(strata 0.1 REQUIRED CONFIG)
if(NOT strata_VERSION STREQUAL EXPECTED_STRATA_VERSION)
    message(FATAL_ERROR
        "found strata ${strata_VERSION}, expected ${EXPECTED_STRATA_VERSION}")
endif()

add_executable(strata_compat_consumer main.cc)
target_link_libraries(strata_compat_consumer PRIVATE strata::strata)
target_compile_definitions(strata_compat_consumer PRIVATE
    STRATA_PACKAGE_VERSION="${strata_VERSION}")
