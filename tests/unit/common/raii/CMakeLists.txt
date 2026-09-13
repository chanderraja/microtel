# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/common/raii/ RAII wrappers (Track D ownership).

microtel_add_test(
    NAME    ssl_ctx_test
    SOURCES ssl_ctx_test.cpp
    LIBS    OpenSSL::SSL)
target_include_directories(ssl_ctx_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    ssl_session_test
    SOURCES ssl_session_test.cpp
    LIBS    OpenSSL::SSL)
target_include_directories(ssl_session_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    nghttp2_session_test
    SOURCES nghttp2_session_test.cpp
    LIBS    PkgConfig::NGHTTP2)
target_include_directories(nghttp2_session_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    inflate_stream_test
    SOURCES inflate_stream_test.cpp
    LIBS    ZLIB::ZLIB)
target_include_directories(inflate_stream_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    deflate_stream_test
    SOURCES deflate_stream_test.cpp
    LIBS    ZLIB::ZLIB)
target_include_directories(deflate_stream_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
