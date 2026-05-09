# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Track D: nghttp2 transport.  EpollReactor first; Http2Transport follows.

add_library(microtel_transport STATIC
    epoll_reactor.cpp)

target_link_libraries(microtel_transport
    PUBLIC  microtel_headers
    PRIVATE microtel_common)

target_include_directories(microtel_transport
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_transport PUBLIC cxx_std_20)
target_compile_options(microtel_transport PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
