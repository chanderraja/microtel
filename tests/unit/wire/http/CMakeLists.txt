# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/wire/http/ (Track B).

microtel_add_test(
    NAME    http_wire_codec_test
    SOURCES http_wire_codec_test.cpp
    LIBS    microtel_http_wire ZLIB::ZLIB)

target_include_directories(http_wire_codec_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
