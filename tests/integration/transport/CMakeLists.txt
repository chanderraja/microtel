# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Integration tests for src/transport/ (Track D).

microtel_add_integration_test(
    NAME    http2_connect_test
    SOURCES http2_connect_test.cpp
    LIBS    microtel_transport)

microtel_add_integration_test(
    NAME    http2_send_test
    SOURCES http2_send_test.cpp
    LIBS    microtel_transport)
