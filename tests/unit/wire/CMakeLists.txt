# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/wire/encoder/ (Track F).

add_subdirectory(http)

microtel_add_test(
    NAME    otlp_encoder_test
    SOURCES otlp_encoder_test.cpp
    LIBS    microtel_encoder microtel_upb_gen)

# Test reaches into src/wire/ for the internal implementation header and into
# gen/ for the upb round-trip parser headers.
target_include_directories(otlp_encoder_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/gen
        ${CMAKE_SOURCE_DIR}/third_party/upb)

# C++20 designated initializers legitimately omit fields that have default
# member initializers. GCC -Wextra fires -Wmissing-field-initializers for
# those — suppress it for this test TU only.
target_compile_options(otlp_encoder_test PRIVATE
    -Wno-missing-field-initializers)
