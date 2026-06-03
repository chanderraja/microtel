# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/sdk/ (Track A). Mirrors src/sdk/ one-to-one per
# tests/unit/README.md.

# M3 — first production-code-driven test in microtel. Drives AlwaysOn
# sampler behaviour against the real production code in src/sdk/.
microtel_add_test(
    NAME    always_on_sampler_test
    SOURCES always_on_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    always_off_sampler_test
    SOURCES always_off_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    trace_id_ratio_sampler_test
    SOURCES trace_id_ratio_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    parent_based_sampler_test
    SOURCES parent_based_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    simple_span_processor_test
    SOURCES simple_span_processor_test.cpp
    LIBS    microtel_sdk)
# Test reaches into src/sdk/simple_span_processor.hpp directly (it's an
# internal header, not a public-API surface). Add src/ to the include path.
target_include_directories(simple_span_processor_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    noop_span_test
    SOURCES noop_span_test.cpp
    LIBS    microtel_sdk)
target_include_directories(noop_span_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    sdk_span_test
    SOURCES sdk_span_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_span_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    sdk_tracer_test
    SOURCES sdk_tracer_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_tracer_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    batch_span_processor_test
    SOURCES batch_span_processor_test.cpp
    LIBS    microtel_sdk)
target_include_directories(batch_span_processor_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M6-C: SdkBuilder::Build() + SdkProvider lifecycle.
microtel_add_test(
    NAME    sdk_builder_test
    SOURCES sdk_builder_test.cpp
    LIBS    microtel_sdk)

# SdkProvider::ForceFlush two-stage flush (processor → exporter).
microtel_add_test(
    NAME    sdk_provider_test
    SOURCES sdk_provider_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_provider_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
