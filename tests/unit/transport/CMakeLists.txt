# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/transport/ (Track D).

microtel_add_test(
    NAME    epoll_reactor_test
    SOURCES epoll_reactor_test.cpp
    LIBS    microtel_transport)

target_include_directories(epoll_reactor_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    http2_transport_test
    SOURCES http2_transport_test.cpp
    LIBS    microtel_transport)

target_include_directories(http2_transport_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
