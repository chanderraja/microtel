# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/sdk/ — Track A. Resource resolution, samplers,
# BatchSpanProcessor / SimpleSpanProcessor, Provider lifecycle.
# See src/sdk/README.md.

add_library(microtel_sdk STATIC
    sampler_factories.cpp
    simple_span_processor.cpp
    noop_span.cpp
    sdk_span.cpp
    sdk_tracer.cpp
    batch_span_processor.cpp)

target_link_libraries(microtel_sdk
    PUBLIC microtel_headers)

target_include_directories(microtel_sdk
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_sdk PUBLIC cxx_std_20)

target_compile_options(microtel_sdk PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
