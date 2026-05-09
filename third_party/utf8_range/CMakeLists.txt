# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Vendored utf8_range (protobuf v29.4 third_party/utf8_range).
# A single-file UTF-8 validator used by upb/wire/decode.c.
# Pin details: third_party/utf8_range/README.md

cmake_minimum_required(VERSION 3.20)

add_library(microtel_utf8_range STATIC utf8_range.c)

target_include_directories(microtel_utf8_range
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(microtel_utf8_range PUBLIC c_std_11)

# Suppress warnings in vendored code.
target_compile_options(microtel_utf8_range PRIVATE -w)
