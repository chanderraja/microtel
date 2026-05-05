# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M0 minimal build: declares a header-only INTERFACE target plus an optional
# header-check executable that includes every public and internal header.
# Implementation begins in M3; see microtel-roadmap.md.

cmake_minimum_required(VERSION 3.20)

project(microtel
        VERSION 0.1.0
        LANGUAGES CXX
        DESCRIPTION "Lightweight OpenTelemetry-compatible trace runtime built on nghttp2")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Header-only target — exposes include/ and the vendored tl::expected to
# consumers and to the M0 header check. Per ICP 0002 the public-API
# `microtel::Expected<T, E>` aliases to `tl::expected` on C++20 and to
# `std::expected` on C++23.
add_library(microtel_headers INTERFACE)
target_include_directories(microtel_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/tl-expected>)
target_compile_features(microtel_headers INTERFACE cxx_std_20)

# M0 header compile check.
#
# Verifies every public and internal header compiles standalone with -Werror.
# Per ICP 0001 this is the operational form of the rule "Both header trees
# compile under an INTERFACE CMake target with -Werror. No implementations."
option(MICROTEL_BUILD_HEADER_CHECK "Build the M0 header compile check" ON)

if(MICROTEL_BUILD_HEADER_CHECK)
    add_executable(microtel_header_check ci/header_check.cpp)
    target_link_libraries(microtel_header_check PRIVATE microtel_headers)
    target_compile_options(microtel_header_check PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# Sanitizer build helper. MICROTEL_SANITIZER ∈ { '', asan, tsan, ubsan }.
# Empty (default) means no sanitizer. Per spec §14.2 / docs/coding-standards.md
# §14.2, asan + tsan + ubsan must be clean on the full test suite for v1.0.
set(MICROTEL_SANITIZER "" CACHE STRING
    "Sanitizer to enable: '', asan, tsan, ubsan")
set_property(CACHE MICROTEL_SANITIZER PROPERTY STRINGS "" asan tsan ubsan)

if(NOT MICROTEL_SANITIZER STREQUAL "")
    if(MICROTEL_SANITIZER STREQUAL "asan")
        set(_microtel_san_flags -fsanitize=address -fno-omit-frame-pointer)
    elseif(MICROTEL_SANITIZER STREQUAL "tsan")
        set(_microtel_san_flags -fsanitize=thread -fno-omit-frame-pointer)
    elseif(MICROTEL_SANITIZER STREQUAL "ubsan")
        set(_microtel_san_flags -fsanitize=undefined -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "MICROTEL_SANITIZER='${MICROTEL_SANITIZER}' invalid; "
            "expected one of: '', asan, tsan, ubsan")
    endif()
    add_compile_options(${_microtel_san_flags})
    add_link_options(${_microtel_san_flags})
    message(STATUS "microtel: ${MICROTEL_SANITIZER} enabled")
endif()

# Coverage build helper. When ON, attaches gcov-style coverage flags so a
# CI job (or local run) can collect line/branch coverage via lcov + gcovr +
# diff-cover. Per spec §14.2, the v1.0 thresholds are 90% line / 85% branch
# on SDK + encoder code, 80% on transport + exporter — enforced once
# implementation lands in M3+.
option(MICROTEL_COVERAGE "Build with --coverage flags for lcov / gcov" OFF)
if(MICROTEL_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
    message(STATUS "microtel: coverage enabled")
endif()

# Test tree — gtest-based unit/integration/conformance/wire/grpc-wire/fuzz
# per spec §14.2 / docs/coding-standards.md §11. Default ON; OFF skips the
# whole tree for cross-compilation or constrained builds (spec §14.2).
option(MICROTEL_BUILD_TESTS "Build the test tree" ON)

if(MICROTEL_BUILD_TESTS)
    enable_testing()

    # GoogleTest via FetchContent — pinned to a stable tag. No system
    # dependency required. Test-only; not part of the runtime closure
    # in microtel-spec.md §9.1.
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    # Don't install gtest as part of microtel's install target.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()
