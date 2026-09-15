# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Integration tests for src/sdk/ wiring that unit tests cannot observe.

microtel_add_integration_test(
    NAME    exporter_health_test
    SOURCES exporter_health_test.cpp
    LIBS    microtel_sdk)

microtel_add_integration_test(
    NAME    retry_budget_test
    SOURCES retry_budget_test.cpp
    LIBS    microtel_sdk)

# Issues #250 / #251: real exporter + real HTTP codec + real auth provider
# against a FakeTransport, so the blast radius of an auth failure is visible.
microtel_add_integration_test(
    NAME    auth_failure_export_test
    SOURCES auth_failure_export_test.cpp
    LIBS    microtel_exporter microtel_http_wire microtel_config)

# Same relaxation as tests/unit/exporter: SpanRecord is built field-by-field.
target_compile_options(auth_failure_export_test PRIVATE
    -Wno-missing-field-initializers)

# Issue #221 / ICP 0025 §3: the two ICurrentSpanSource seams — metrics
# exemplars and log trace-correlation — driven through a real SdkProvider.
microtel_add_integration_test(
    NAME    exemplar_wiring_test
    SOURCES exemplar_wiring_test.cpp
    LIBS    microtel_sdk)

# SdkProviderArgs and LogRecord are built with designated initializers that
# intentionally leave later fields at their defaults.
target_compile_options(exemplar_wiring_test PRIVATE
    -Wno-missing-field-initializers)
