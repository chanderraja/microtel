# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/wire/encoder/ — OtlpEncoder: the only TU that links upb headers.
# Produces the microtel_encoder static library consumed by the exporter.

add_library(microtel_encoder STATIC
    otlp_encoder.cpp)

target_link_libraries(microtel_encoder
    PUBLIC  microtel_headers
    PRIVATE microtel_upb_gen)

# otlp_encoder.cpp includes upb headers via relative paths rooted at
# third_party/upb and the generated accessors rooted at gen/.
target_include_directories(microtel_encoder
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/gen
        ${CMAKE_SOURCE_DIR}/third_party/upb)

target_compile_features(microtel_encoder PUBLIC cxx_std_20)

target_compile_options(microtel_encoder PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
