# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/wire/grpc/ — OTLP/gRPC implementation of IWireCodec (M4).

add_library(microtel_grpc_wire STATIC grpc_wire_codec.cpp)

target_link_libraries(microtel_grpc_wire PUBLIC microtel_headers microtel_otlp_response microtel_gzip)

target_include_directories(microtel_grpc_wire PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_grpc_wire PUBLIC cxx_std_20)

target_compile_options(microtel_grpc_wire PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
