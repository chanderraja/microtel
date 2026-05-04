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
