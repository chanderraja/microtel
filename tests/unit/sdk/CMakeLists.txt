# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/sdk/ (Track A). Mirrors src/sdk/ one-to-one per
# tests/unit/README.md.

# M3 — first production-code-driven test in microtel. Drives AlwaysOn
# sampler behaviour against the real production code in src/sdk/.
microtel_add_test(
    NAME    always_on_sampler_test
    SOURCES always_on_sampler_test.cpp
    LIBS    microtel_sdk)

# M12 — metric aggregation key (AttributeSet).
microtel_add_test(
    NAME    metric_attribute_set_test
    SOURCES metric_attribute_set_test.cpp
    LIBS    microtel_sdk)
# Reaches into src/sdk/metric_attribute_set.hpp directly (internal header).
target_include_directories(metric_attribute_set_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — Sum aggregation storage.
microtel_add_test(
    NAME    metric_sum_storage_test
    SOURCES metric_sum_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_sum_storage_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — Gauge (last-value) aggregation storage.
microtel_add_test(
    NAME    metric_gauge_storage_test
    SOURCES metric_gauge_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_gauge_storage_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — explicit-bucket Histogram aggregation storage.
microtel_add_test(
    NAME    metric_histogram_storage_test
    SOURCES metric_histogram_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_histogram_storage_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — base-2 exponential Histogram aggregation storage.
microtel_add_test(
    NAME    metric_exp_histogram_storage_test
    SOURCES metric_exp_histogram_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_exp_histogram_storage_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — MetricProducer: IMetricProducer impl that assembles MetricBatchHandles.
microtel_add_test(
    NAME    metric_producer_test
    SOURCES metric_producer_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_producer_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — Concrete IMetricStream implementations (one per storage kind).
microtel_add_test(
    NAME    metric_stream_impls_test
    SOURCES metric_stream_impls_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_stream_impls_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — Meter: instrument factory wrapping MetricProducer.
microtel_add_test(
    NAME    meter_test
    SOURCES meter_test.cpp
    LIBS    microtel_sdk)
target_include_directories(meter_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — Observable instruments (callback-driven streams + Meter factories).
microtel_add_test(
    NAME    metric_observable_instruments_test
    SOURCES metric_observable_instruments_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_observable_instruments_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — PeriodicExportingMetricReader: background-thread push-model reader.
microtel_add_test(
    NAME    periodic_exporting_metric_reader_test
    SOURCES periodic_exporting_metric_reader_test.cpp
    LIBS    microtel_sdk)
target_include_directories(periodic_exporting_metric_reader_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — SynchronousMetricReader: collect + export pipeline (pull-on-demand).
microtel_add_test(
    NAME    synchronous_metric_reader_test
    SOURCES synchronous_metric_reader_test.cpp
    LIBS    microtel_sdk)
target_include_directories(synchronous_metric_reader_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    always_off_sampler_test
    SOURCES always_off_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    trace_id_ratio_sampler_test
    SOURCES trace_id_ratio_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    parent_based_sampler_test
    SOURCES parent_based_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    simple_span_processor_test
    SOURCES simple_span_processor_test.cpp
    LIBS    microtel_sdk)
# Test reaches into src/sdk/simple_span_processor.hpp directly (it's an
# internal header, not a public-API surface). Add src/ to the include path.
target_include_directories(simple_span_processor_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    noop_span_test
    SOURCES noop_span_test.cpp
    LIBS    microtel_sdk)
target_include_directories(noop_span_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    sdk_span_test
    SOURCES sdk_span_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_span_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    sdk_tracer_test
    SOURCES sdk_tracer_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_tracer_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

microtel_add_test(
    NAME    batch_span_processor_test
    SOURCES batch_span_processor_test.cpp
    LIBS    microtel_sdk)
target_include_directories(batch_span_processor_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — SdkProvider::GetMeter: meter factory + scope caching.
microtel_add_test(
    NAME    sdk_provider_meter_test
    SOURCES sdk_provider_meter_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_provider_meter_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

target_compile_options(sdk_provider_meter_test PRIVATE
    -Wno-missing-field-initializers)

# M6-C: SdkBuilder::Build() + SdkProvider lifecycle.
microtel_add_test(
    NAME    sdk_builder_test
    SOURCES sdk_builder_test.cpp
    LIBS    microtel_sdk)

# SdkProvider::ForceFlush two-stage flush (processor → exporter).
microtel_add_test(
    NAME    sdk_provider_test
    SOURCES sdk_provider_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_provider_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests)

target_compile_options(sdk_provider_test PRIVATE
    -Wno-missing-field-initializers)

# M12 — ICP 0008 metric drop reasons + diagnostics name mapping.
microtel_add_test(
    NAME    drop_reason_test
    SOURCES drop_reason_test.cpp
    LIBS    microtel_sdk)
target_include_directories(drop_reason_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)

# M12 — DiagnosticsCounters: production IDiagnosticsSink (atomic counters).
microtel_add_test(
    NAME    diagnostics_counters_test
    SOURCES diagnostics_counters_test.cpp
    LIBS    microtel_sdk)
target_include_directories(diagnostics_counters_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
