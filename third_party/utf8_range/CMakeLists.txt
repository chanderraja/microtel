# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Vendored utf8_range (protobuf v29.4 third_party/utf8_range).
# A single-file UTF-8 validator used by upb/wire/decode.c.
# Pin details: third_party/utf8_range/README.md

cmake_minimum_required(VERSION 3.20)

add_library(microtel_utf8_range STATIC utf8_range.c)

# BUILD_INTERFACE only: the archive is installed so the static link closure
# resolves, but no vendored header ever reaches a consumer — every upb include
# path in this project is PRIVATE. A bare path here would also be a stale
# absolute source path in the installed export file.
target_include_directories(microtel_utf8_range
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_features(microtel_utf8_range PUBLIC c_std_11)

# -include first: the symbol-prefix header must be seen before any TU text.
# See third_party/upb/microtel_upb_rename.h and ICP 0020 Decision 4.
# -w suppresses warnings in vendored code.
target_compile_options(microtel_utf8_range PRIVATE
    -include ${MICROTEL_UPB_RENAME_HEADER}
    -w)
