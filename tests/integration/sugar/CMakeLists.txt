# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/integration/sugar/ — the single sugar integration test (ICP 0028 §3).
#
# Sugar is excluded from conformance testing by microtel-spec.md §18.1, so
# this one test is the whole of its non-unit coverage: a sugar-written call
# tree exported through a real SdkTracer + BatchSpanProcessor.

microtel_add_integration_test(
    NAME    sugar_pipeline_test
    SOURCES sugar_pipeline_test.cpp
    LIBS    microtel_sdk)

# SdkProviderArgs is built with designated initializers that intentionally
# leave the metrics and logs fields at their defaults — same relaxation as
# the other SdkProvider-driven integration tests.
target_compile_options(sugar_pipeline_test PRIVATE
    -Wno-missing-field-initializers)
