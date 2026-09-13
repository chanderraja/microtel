# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Vendored upb runtime (protobuf v29.4).
# Pin details and refresh procedure: third_party/upb/README.md
#
# Builds two OBJECT/STATIC libraries consumed by microtel_upb_gen
# (gen/CMakeLists.txt) and ultimately by microtel_encoder (src/wire/encoder/).
#
# The archive is installed and exported as `microtel::upb_runtime` — a static
# link closure needs it — but the headers are not, and the target is
# exported-but-unsupported (ICP 0020 Decision 2). The install rules live in the
# top-level CMakeLists.txt, which keeps the shipped set in one place.

cmake_minimum_required(VERSION 3.20)

# utf8_range is added by the top-level CMakeLists before this subdir.
# We consume it via target_link_libraries below — no add_subdirectory here.

# --- upb runtime ---
add_library(microtel_upb_runtime STATIC
    upb/base/status.c
    upb/hash/common.c
    upb/mem/alloc.c
    upb/mem/arena.c
    upb/message/accessors.c
    upb/message/array.c
    upb/message/compare.c
    upb/message/compat.c
    upb/message/copy.c
    upb/message/internal/compare_unknown.c
    upb/message/internal/extension.c
    upb/message/internal/iterator.c
    upb/message/internal/message.c
    upb/message/map.c
    upb/message/map_sorter.c
    upb/message/merge.c
    upb/message/message.c
    upb/message/promote.c
    upb/mini_descriptor/build_enum.c
    upb/mini_descriptor/decode.c
    upb/mini_descriptor/internal/base92.c
    upb/mini_descriptor/internal/encode.c
    upb/mini_descriptor/link.c
    upb/mini_table/compat.c
    upb/mini_table/extension_registry.c
    upb/mini_table/internal/message.c
    upb/mini_table/message.c
    upb/wire/byte_size.c
    upb/wire/decode.c
    upb/wire/encode.c
    upb/wire/eps_copy_input_stream.c
    upb/wire/internal/decode_fast.c
    upb/wire/reader.c)

# Headers use #include "upb/...", resolved from this directory.
# utf8_range.h is found via the utf8_range include path below.
#
# BUILD_INTERFACE only — see the note in third_party/utf8_range/CMakeLists.txt.
# The archive installs; the headers do not.
target_include_directories(microtel_upb_runtime
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/utf8_range>)

target_link_libraries(microtel_upb_runtime PUBLIC microtel_utf8_range)

# upb is C99/C11; no C++ standard required.
target_compile_features(microtel_upb_runtime PUBLIC c_std_11)

# -include first: the symbol-prefix header must be seen before any TU text.
# See microtel_upb_rename.h and ICP 0020 Decision 4.
# -w suppresses warnings in vendored code — we don't own this source.
target_compile_options(microtel_upb_runtime PRIVATE
    -include ${MICROTEL_UPB_RENAME_HEADER}
    -w)
