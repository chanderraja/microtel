# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# microtel-leaf: the C span builder and OTLP encoder for constrained devices
# (docs/leaf-concentrator-design.md §1, §2.1).
#
# Works two ways:
#
#   - standalone:   cmake -S leaf -B build-leaf -DMICROTEL_LEAF_ENCODER=upb
#     Needs only a C compiler. Never enables CXX and never looks for nghttp2,
#     OpenSSL or zlib, so a cross toolchain with no C++ runtime can build it.
#   - in-tree:      cmake -S . -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_LEAF_ENCODER=upb
#     The top-level project adds this directory. `microtel_leaf` is exported
#     as `microtel::leaf` and is never a dependency of `microtel::microtel`.
#
# The upb backend links the project's own microtel_upb_gen / microtel_upb_runtime
# / microtel_utf8_range archives rather than compiling a second copy, so a process
# that links both the runtime and a upb leaf gets one upb.

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(microtel_leaf
            VERSION 1.1.1
            LANGUAGES C
            DESCRIPTION "microtel-leaf: OTLP span encoder for constrained devices")
    set(MICROTEL_LEAF_STANDALONE ON)
    include(GNUInstallDirs)
else()
    set(MICROTEL_LEAF_STANDALONE OFF)
endif()

# In-tree the top-level CMakeLists defines (and validates) this option; the
# definition here only takes effect standalone. Same default: nanopb (§6.1).
set(MICROTEL_LEAF_ENCODER "nanopb" CACHE STRING "Leaf encoder backend: upb or nanopb")
set_property(CACHE MICROTEL_LEAF_ENCODER PROPERTY STRINGS upb nanopb)

# The nanopb backend is not written yet. A nanopb leaf never silently falls
# back to upb: standalone there is nothing else to build, so it is an error; in
# the main project the vendored nanopb archives and their tests still build,
# and microtel_leaf is left out with a warning.
set(_microtel_leaf_nanopb_msg
    "MICROTEL_LEAF_ENCODER=nanopb: the nanopb leaf backend is not implemented yet, "
    "so microtel_leaf is not built. Configure with -DMICROTEL_LEAF_ENCODER=upb.")
if(MICROTEL_LEAF_ENCODER STREQUAL "nanopb")
    if(MICROTEL_LEAF_STANDALONE)
        message(FATAL_ERROR ${_microtel_leaf_nanopb_msg})
    endif()
    message(WARNING ${_microtel_leaf_nanopb_msg})
    return()
elseif(NOT MICROTEL_LEAF_ENCODER STREQUAL "upb")
    message(FATAL_ERROR
        "MICROTEL_LEAF_ENCODER='${MICROTEL_LEAF_ENCODER}' invalid; expected upb or nanopb")
endif()

# Standalone: build the vendored upb archives from the same sources the main
# project uses. In-tree they already exist.
if(NOT TARGET microtel_upb_gen)
    set(MICROTEL_UPB_RENAME_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/upb/microtel_upb_rename.h)
    add_subdirectory(../third_party/utf8_range ${CMAKE_CURRENT_BINARY_DIR}/utf8_range)
    add_subdirectory(../third_party/upb ${CMAKE_CURRENT_BINARY_DIR}/upb)
    add_subdirectory(../gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
endif()

add_library(microtel_leaf STATIC
    src/leaf_core.c
    src/backend_upb.c)

target_include_directories(microtel_leaf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The backend entry point leaf_core.c calls (§2.1).
target_compile_definitions(microtel_leaf PRIVATE
    MICROTEL_LEAF_BACKEND_ENCODE=microtel_leaf_internal_encode_upb)

# C11 with no extensions (§1.2).
set_target_properties(microtel_leaf PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(microtel_leaf PRIVATE
        -pedantic-errors -Wall -Wextra -Wvla -Werror)
endif()

# Only backend_upb.c includes upb headers; it needs the rename header forced in
# so it calls the renamed runtime (ICP 0020 Decision 4).
set_source_files_properties(src/backend_upb.c PROPERTIES
    COMPILE_OPTIONS "-include;${MICROTEL_UPB_RENAME_HEADER}")

# PRIVATE: the upb include paths never reach a consumer. A static archive still
# carries the link dependency, so the closure resolves.
target_link_libraries(microtel_leaf PRIVATE microtel_upb_gen)

set_target_properties(microtel_leaf PROPERTIES EXPORT_NAME leaf)
add_library(microtel::leaf ALIAS microtel_leaf)

if(MICROTEL_LEAF_STANDALONE)
    install(TARGETS microtel_leaf microtel_upb_gen microtel_upb_runtime microtel_utf8_range
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/microtel/leaf.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microtel)
endif()
