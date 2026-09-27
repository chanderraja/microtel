# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/wire/custom/ — the IWireCodec over an application-supplied
# ExportTransport (ICP 0036).

add_library(microtel_custom_wire STATIC
    export_transport_codec.cpp)

# microtel_common carries the internal log entry point the oversized-request
# warn goes through.
target_link_libraries(microtel_custom_wire
    PUBLIC microtel_headers microtel_common)

target_include_directories(microtel_custom_wire PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_custom_wire PUBLIC cxx_std_20)

target_compile_options(microtel_custom_wire PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
