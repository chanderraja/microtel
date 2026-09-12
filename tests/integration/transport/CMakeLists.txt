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

# OpenSSL is linked explicitly: microtel_transport keeps it PRIVATE, and this
# test builds its own TLS server rather than borrowing the transport's.
microtel_add_integration_test(
    NAME    http2_tls_connect_test
    SOURCES http2_tls_connect_test.cpp
    LIBS    microtel_transport OpenSSL::SSL OpenSSL::Crypto)
