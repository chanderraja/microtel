# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M17 L5 — wire conformance: telemetry driven through the otel-cpp API,
# registered onto a real SdkBuilder-built Provider, over a real loopback
# HTTP/2 connection. Opt-in, same guard as the rest of the shim tree.

if(MICROTEL_BUILD_OTELCPP_SHIM)
    microtel_add_integration_test(
        NAME    otelcpp_wire_conformance_test
        SOURCES wire_conformance_test.cpp
        LIBS    microtel_otelcpp_shim microtel_sdk microtel_encoder microtel_upb_gen)

    # Reaches into src/adapters/otelcpp/ for the shim headers and into gen/
    # for the upb round-trip parser headers (same pattern as
    # tests/unit/wire/otlp_encoder_test.cpp).
    target_include_directories(otelcpp_wire_conformance_test
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/gen
            ${CMAKE_SOURCE_DIR}/third_party/upb)

    target_compile_options(otelcpp_wire_conformance_test PRIVATE
        -Wno-missing-field-initializers)
endif()
