# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer harnesses — built only when -DMICROTEL_BUILD_FUZZ=ON.
# Requires clang with -fsanitize=fuzzer (libFuzzer is clang-only).
#
# These targets are NOT registered with ctest. Fuzzing runs under the
# periodic fuzz.yml CI job and locally. The crashes/ corpus replay IS a
# hard PR gate (see ci/scripts/corpus-check.sh).
#
# Usage:
#   cmake -S . -B build-fuzz \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DMICROTEL_BUILD_FUZZ=ON -DMICROTEL_BUILD_TESTS=OFF
#   cmake --build build-fuzz
#   ./build-fuzz/tests/fuzz/toml_fuzz corpus/toml_fuzz/ -max_total_time=60

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(WARNING
        "MICROTEL_BUILD_FUZZ=ON requires clang "
        "(CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}). "
        "Fuzz targets will not be built.")
    return()
endif()

function(microtel_add_fuzz_target)
    cmake_parse_arguments(MF "" "NAME" "SOURCES;LIBS" ${ARGN})

    if(NOT MF_NAME OR NOT MF_SOURCES)
        message(FATAL_ERROR "microtel_add_fuzz_target: NAME and SOURCES are required")
    endif()

    add_executable(${MF_NAME} ${MF_SOURCES})

    target_compile_options(${MF_NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -Wall -Wextra -Wpedantic -Werror)

    target_link_options(${MF_NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined)

    target_link_libraries(${MF_NAME} PRIVATE
        microtel_headers
        ${MF_LIBS})

    target_include_directories(${MF_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)
endfunction()

microtel_add_fuzz_target(
    NAME    grpc_codec_fuzz
    SOURCES grpc_codec_fuzz.cpp
    LIBS    microtel_grpc_wire)

microtel_add_fuzz_target(
    NAME    toml_fuzz
    SOURCES toml_fuzz.cpp
    LIBS    microtel_config)

microtel_add_fuzz_target(
    NAME    otlp_response_fuzz
    SOURCES otlp_response_fuzz.cpp
    LIBS    microtel_otlp_response)

microtel_add_fuzz_target(
    NAME    response_decompression_fuzz
    SOURCES response_decompression_fuzz.cpp)
