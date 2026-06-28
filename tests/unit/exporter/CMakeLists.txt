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

target_compile_options(otlp_exporter_test PRIVATE
    -Wno-missing-field-initializers)

microtel_add_test(
    NAME    retry_policy_test
    SOURCES retry_policy_test.cpp
    LIBS    microtel_exporter)

target_include_directories(retry_policy_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    otlp_metric_exporter_test
    SOURCES otlp_metric_exporter_test.cpp
    LIBS    microtel_exporter)

target_include_directories(otlp_metric_exporter_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

target_compile_options(otlp_metric_exporter_test PRIVATE
    -Wno-missing-field-initializers)
