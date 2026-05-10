# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M0 minimal build: declares a header-only INTERFACE target plus an optional
# header-check executable that includes every public and internal header.
# Implementation begins in M3; see microtel-roadmap.md.

cmake_minimum_required(VERSION 3.20)

project(microtel
        VERSION 0.1.0
        LANGUAGES C CXX
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
    # `-fprofile-update=atomic` makes the gcov counter increments atomic.
    # Without it, multi-threaded tests (log_sink_test, future
    # BatchSpanProcessor tests) race on shared counters and produce
    # corrupted .gcda files — lcov surfaces this as a negative delta.
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
    message(STATUS "microtel: coverage enabled")
endif()

# spdlog for internal diagnostic logging. Default ON per spec §9.2 + §9.4;
# OFF uses the minimal stderr fallback. Either way, the public LogSink
# hook in include/microtel/log_sink.hpp lets applications redirect into
# their own logger (per docs/error-model.md §9.3).
option(MICROTEL_USE_SPDLOG "Use spdlog for internal diagnostic logs" ON)

if(MICROTEL_USE_SPDLOG)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.15.3
        GIT_SHALLOW    TRUE
    )
    # std::format mode — avoids the fmt transitive dependency that
    # microtel-spec.md §9.4 explicitly excludes.
    set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

# Third-party libraries: upb runtime + utf8_range validator.
# CMake targets: microtel_upb_runtime, microtel_utf8_range.
# These are compiled as C11 with -w (no warnings from vendored code).
add_subdirectory(third_party/utf8_range)
add_subdirectory(third_party/upb)

# Generated upb accessors (protoc-gen-upb output committed under gen/).
# CMake target: microtel_upb_gen. Regen: ci/scripts/regen-protos.sh.
add_subdirectory(gen)

# System dependencies: OpenSSL (TLS) and nghttp2 (HTTP/2 transport).
# Required by src/common/raii/ and src/transport/; also linked by their tests.
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

# src/ — production code per the track ownership in
# `docs/development.md`. Subdirectories grow as M3+ implementation
# chunks land. Track A (src/sdk/) leads M3.
add_subdirectory(src/common)
add_subdirectory(src/sdk)
add_subdirectory(src/wire)
add_subdirectory(src/transport)
add_subdirectory(src/exporter)

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
