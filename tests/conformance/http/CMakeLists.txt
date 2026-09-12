# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# OTLP/HTTP-protobuf against a real collector.

microtel_add_conformance_test(
    NAME    conformance_http_basic_test
    SOURCES basic_export_test.cpp
    LIBS    microtel_sdk)
