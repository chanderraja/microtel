# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M1 spike build — throwaway code that validates M0's architecture
# decisions against a real OpenTelemetry Collector. This subdirectory
# is gated behind MICROTEL_BUILD_SPIKE=ON so M0/M2/M3+ builds don't
# accidentally pull in spike binaries or their nghttp2/OpenSSL deps.
#
# Per spike/README.md, this directory is deleted at the end of M1.
# The CMake configuration is intentionally minimal — it isn't shipped.

find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)
find_package(OpenSSL REQUIRED)

# Path of the shared protoc-encoded ExportTraceServiceRequest fixture.
# Both spike binaries read this same file; see spike/fixtures/README.md.
set(MICROTEL_SPIKE_FIXTURE_BIN
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/single_span.bin")

# Common settings for every spike binary.
function(microtel_add_spike_binary target)
    target_link_libraries(${target} PRIVATE
        ${NGHTTP2_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto)
    target_include_directories(${target} PRIVATE ${NGHTTP2_INCLUDE_DIRS})
    target_compile_features(${target} PRIVATE cxx_std_20)
    # Spike code follows the same warning posture as the M0 header check.
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    # Embed the path of the shared encoded fixture as a compile-time string.
    target_compile_definitions(${target} PRIVATE
        MICROTEL_SPIKE_FIXTURE="${MICROTEL_SPIKE_FIXTURE_BIN}")
endfunction()

# OTLP/HTTP-protobuf spike: single span, end-to-end against a local collector.
add_executable(spike_http_post  http_post/main.cpp)
microtel_add_spike_binary(spike_http_post)

# OTLP/gRPC-on-nghttp2 spike: 5-byte framing, trailer parsing, no gRPC library.
add_executable(spike_grpc_unary grpc_unary/main.cpp)
microtel_add_spike_binary(spike_grpc_unary)

message(STATUS "M1 spike enabled — building spike_http_post and spike_grpc_unary")
message(STATUS "  Run docker compose -f spike/docker/otel-collector.yml up -d before executing")
