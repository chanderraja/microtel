# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Vendored nanopb runtime (nanopb-0.4.9.2), encoder only.
# Pin details and refresh procedure: third_party/nanopb/README.md
#
# nanopb is a dependency of the leaf only (ICP 0031 Decision 4): it is built
# when the leaf is built with MICROTEL_LEAF_ENCODER=nanopb and never enters
# microtel::microtel or any other shipped archive. symbol-scan.sh fails any
# non-leaf archive that references a pb_* or microtel_pb_* symbol.
#
# This file is C-only and reads no variable of the main project, so the leaf's
# standalone build (`cmake -S leaf`, docs/leaf-concentrator-design.md §2.1) can
# add it with add_subdirectory() and a binary directory of its own.

cmake_minimum_required(VERSION 3.20)

# pb_decode.c is not vendored: the leaf only encodes (design §2.1). Every
# decode on the concentrator side is upb.
add_library(microtel_nanopb STATIC
    pb_common.c
    pb_encode.c)

target_include_directories(microtel_nanopb
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

set_target_properties(microtel_nanopb PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

# Configuration macros change the layout of pb_ostream_t (PB_NO_ERRMSG drops
# its errmsg member), so they are PUBLIC: every TU that includes pb.h must see
# the same set as the archive was built with. Design §2.4:
#   PB_NO_ERRMSG     no error strings in flash.
#   not defined:     PB_BUFFER_ONLY (the streaming encode needs callback
#                    streams), PB_WITHOUT_64BIT (OTLP timestamps are fixed64),
#                    PB_ENABLE_MALLOC (no field is FT_POINTER).
target_compile_definitions(microtel_nanopb PUBLIC PB_NO_ERRMSG)

# Every nanopb global is renamed to microtel_pb_* (ICP 0031 Decision 4, design
# §2.5) by a force-included header, as microtel_upb_rename.h does for upb. It
# is PUBLIC, unlike upb's, because every TU that includes a nanopb or
# generated-nanopb header must see the rename or it references the upstream
# names and fails to link; propagating it through the target makes that
# automatic for the leaf's nanopb backend and for tests. BUILD_INTERFACE only:
# nothing that consumes an installed leaf includes a nanopb header.
#
# -w suppresses warnings in vendored code; it stays PRIVATE.
target_compile_options(microtel_nanopb
    PUBLIC $<BUILD_INTERFACE:-include${CMAKE_CURRENT_SOURCE_DIR}/microtel_pb_rename.h>
    PRIVATE -w)
