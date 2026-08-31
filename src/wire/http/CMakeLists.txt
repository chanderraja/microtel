# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/wire/http/ — OTLP/HTTP-protobuf IWireCodec implementation (Track B).

add_library(microtel_http_wire STATIC
    http_wire_codec.cpp)

target_link_libraries(microtel_http_wire
    PUBLIC microtel_headers microtel_otlp_response microtel_gzip)

target_include_directories(microtel_http_wire
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_http_wire PUBLIC cxx_std_20)

target_compile_options(microtel_http_wire PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
