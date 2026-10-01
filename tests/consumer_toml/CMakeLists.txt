# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/consumer_toml — an installed-microtel consumer that also links a
# *compiled* toml++ of its own.
#
# microtel compiles toml++ header-only (TOML_HEADER_ONLY=1) into
# libmicrotel_config.a, so the installed package needs no toml++ library. An
# application that uses a packaged toml++ built as a library (vcpkg's is;
# its target forces TOML_HEADER_ONLY=0) then has two copies of the same
# toml++ code in one link: microtel's weak inline definitions and the
# library's strong ones, with identical mangled names (toml++'s ABI namespace
# encodes TOML_EXCEPTIONS, not TOML_HEADER_ONLY). This project proves that
# links and runs, with both copies parsing in one process.
#
# Like tests/consumer, NOT part of the microtel build. Driven by
# ci/scripts/consumer-smoke.sh when CONSUMER_TOML=1. By hand:
#
#   cmake -S tests/consumer_toml -B /tmp/mt-consumer-toml \
#         -DCMAKE_PREFIX_PATH="/tmp/mt-prefix;<prefix holding tomlplusplus>"
#   cmake --build /tmp/mt-consumer-toml
#   /tmp/mt-consumer-toml/microtel_consumer_toml

cmake_minimum_required(VERSION 3.20)

project(microtel_consumer_toml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(microtel REQUIRED CONFIG)
find_package(tomlplusplus REQUIRED CONFIG)

# A header-only toml++ here would make this a second copy of the in-tree
# config tests, not a coexistence test: refuse it rather than pass vacuously.
get_target_property(_toml_type tomlplusplus::tomlplusplus TYPE)
if(_toml_type STREQUAL "INTERFACE_LIBRARY")
    message(FATAL_ERROR
            "consumer_toml: tomlplusplus::tomlplusplus at ${tomlplusplus_DIR} is "
            "header-only. This test needs toml++ built as a library (for example "
            "vcpkg's tomlplusplus port).")
endif()

add_executable(microtel_consumer_toml main.cpp)
target_link_libraries(microtel_consumer_toml PRIVATE microtel::microtel tomlplusplus::tomlplusplus)

message(STATUS "microtel_DIR=${microtel_DIR} tomlplusplus_DIR=${tomlplusplus_DIR} "
               "tomlplusplus type=${_toml_type}")
