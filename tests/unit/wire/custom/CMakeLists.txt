# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/wire/custom/ (ICP 0036).

microtel_add_test(
    NAME    export_transport_codec_test
    SOURCES export_transport_codec_test.cpp
    LIBS    microtel_custom_wire)

target_include_directories(export_transport_codec_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

target_compile_options(export_transport_codec_test PRIVATE
    -Wno-missing-field-initializers)
