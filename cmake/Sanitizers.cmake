# STRATA_SANITIZE is a comma-separated -fsanitize= list applied globally,
# e.g. -DSTRATA_SANITIZE=address,undefined (the `dev` preset sets this).
set(STRATA_SANITIZE "" CACHE STRING "Comma-separated sanitizer list (address,undefined,thread)")
if(NOT STRATA_SANITIZE STREQUAL "")
    add_compile_options(-fsanitize=${STRATA_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${STRATA_SANITIZE})
endif()
