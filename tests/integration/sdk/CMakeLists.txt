# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Integration tests for src/sdk/ wiring that unit tests cannot observe.

microtel_add_integration_test(
    NAME    exporter_health_test
    SOURCES exporter_health_test.cpp
    LIBS    microtel_sdk)
