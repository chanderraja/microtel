# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/exporter/ (M3 close).

microtel_add_test(
    NAME    otlp_exporter_test
    SOURCES otlp_exporter_test.cpp
    LIBS    microtel_exporter)

target_include_directories(otlp_exporter_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

# Designated initializers with partial fields on aggregate structs (e.g.
# SpanRecord, WireResult) — suppressed for test TUs only; production code
# initialises all fields explicitly.
target_compile_options(otlp_exporter_test PRIVATE
    -Wno-missing-field-initializers)
