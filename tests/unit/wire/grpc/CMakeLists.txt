# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/wire/grpc/ (M4).

microtel_add_test(
    NAME    grpc_wire_codec_test
    SOURCES grpc_wire_codec_test.cpp
    LIBS    microtel_grpc_wire)

target_include_directories(grpc_wire_codec_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

target_compile_options(grpc_wire_codec_test PRIVATE
    -Wno-missing-field-initializers)
