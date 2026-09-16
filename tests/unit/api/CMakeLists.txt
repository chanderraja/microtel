# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/unit/api/ — public API contract tests exercised through the abstract
# microtel::Provider / microtel::Meter interfaces (not SDK internals directly).

microtel_add_test(
    NAME    trace_id_test
    SOURCES trace_id_test.cpp
    LIBS    microtel_api)

microtel_add_test(
    NAME    trace_state_test
    SOURCES trace_state_test.cpp
    LIBS    microtel_api)

microtel_add_test(
    NAME    propagator_test
    SOURCES propagator_test.cpp
    LIBS    microtel_api)

# v1.1 packet 2.3b — the thread-local current-context slot and ScopedContext
# (issue #221, ICP 0025 §3). The slot lives in microtel_api's context.cpp.
microtel_add_test(
    NAME    context_test
    SOURCES context_test.cpp
    LIBS    microtel_api)

# v1.1 packet 2.3c — W3C Baggage: the grammar, percent-encoding, the property
# tail, and the three limits (ICP 0025 §2). Lives in microtel_api's baggage.cpp.
microtel_add_test(
    NAME    baggage_test
    SOURCES baggage_test.cpp
    LIBS    microtel_api)

# v1.1 packet 2.3c — W3CBaggagePropagator and the Context baggage slot it
# fills (ICP 0025 §2, §4).
microtel_add_test(
    NAME    baggage_propagator_test
    SOURCES baggage_propagator_test.cpp
    LIBS    microtel_api)

# v1.1 — Resource::Merge precedence (spec §12.7). Header-only: the merge is an
# inline member of the public `Resource`, so this needs no library beyond the
# header target microtel_add_test already links.
microtel_add_test(
    NAME    resource_merge_test
    SOURCES resource_merge_test.cpp)

microtel_add_test(
    NAME    meter_api_test
    SOURCES meter_api_test.cpp
    LIBS    microtel_sdk)
target_include_directories(meter_api_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
# SdkProviderArgs uses designated initializers; new fields added in M13 are
# intentionally omitted here (defaults are sufficient for these API tests).
target_compile_options(meter_api_test PRIVATE -Wno-missing-field-initializers)
