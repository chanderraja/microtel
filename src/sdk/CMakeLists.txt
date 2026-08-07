# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/sdk/ — Track A. Resource resolution, samplers,
# BatchSpanProcessor / SimpleSpanProcessor, Provider lifecycle.
# See src/sdk/README.md.

add_library(microtel_sdk STATIC
    diagnostics_counters.cpp
    sampler_factories.cpp
    metric_attribute_set.cpp
    metric_sum_storage.cpp
    metric_gauge_storage.cpp
    metric_histogram_storage.cpp
    metric_exp_histogram_storage.cpp
    metric_producer.cpp
    synchronous_metric_reader.cpp
    periodic_exporting_metric_reader.cpp
    sdk_meter.cpp
    simple_span_processor.cpp
    noop_span.cpp
    sdk_span.cpp
    sdk_tracer.cpp
    sdk_logger.cpp
    batch_span_processor.cpp
    sdk_provider.cpp
    sdk_builder.cpp
    view_registry.cpp)

target_link_libraries(microtel_sdk
    PUBLIC  microtel_headers
    PRIVATE microtel_config
            microtel_exporter
            microtel_transport
            microtel_http_wire
            microtel_grpc_wire
            microtel_encoder)

target_include_directories(microtel_sdk
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_features(microtel_sdk PUBLIC cxx_std_20)

target_compile_options(microtel_sdk PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
