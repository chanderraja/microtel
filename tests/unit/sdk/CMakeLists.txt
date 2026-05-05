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
