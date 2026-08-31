# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/wire/ — wire encoding and transport.
# Track F: encoder. Track B/D: HTTP + gRPC transport (M3+).

add_library(microtel_otlp_response STATIC otlp_response.cpp)

target_link_libraries(microtel_otlp_response PUBLIC microtel_headers)

target_include_directories(microtel_otlp_response PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_otlp_response PUBLIC cxx_std_20)

target_compile_options(microtel_otlp_response PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

# gzip request compression, shared by the HTTP and gRPC codecs.
add_library(microtel_gzip STATIC gzip.cpp)

target_link_libraries(microtel_gzip PUBLIC microtel_headers PRIVATE ZLIB::ZLIB)

target_include_directories(microtel_gzip PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_gzip PUBLIC cxx_std_20)

# ZLIB_CONST makes z_stream::next_in a `const Bytef*`, so compressing a
# borrowed span needs no const_cast.
target_compile_definitions(microtel_gzip PRIVATE ZLIB_CONST)

target_compile_options(microtel_gzip PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_subdirectory(encoder)
add_subdirectory(http)
add_subdirectory(grpc)
