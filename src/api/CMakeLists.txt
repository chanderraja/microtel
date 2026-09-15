# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/api/ — definitions behind the public API headers (Track A).
# See src/api/README.md.

add_library(microtel_api STATIC
    propagator.cpp
    trace.cpp
    trace_state.cpp)

target_link_libraries(microtel_api
    PUBLIC microtel_headers)

target_include_directories(microtel_api
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_api PUBLIC cxx_std_20)

target_compile_options(microtel_api PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
