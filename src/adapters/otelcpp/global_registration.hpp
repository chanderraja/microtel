// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"

#include "adapters/otelcpp/logger_shim.hpp"
#include "adapters/otelcpp/meter_shim.hpp"
#include "adapters/otelcpp/tracer_shim.hpp"

#include <memory>
#include <utility>

#include <opentelemetry/logs/noop.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>

/// @file
/// The shim's single startup call: registers a `microtel::Provider` as the
/// global otel-cpp provider for all three signals at once, so already-
/// instrumented application code needs exactly one line to switch onto
/// microtel (ICP 0014's stated goal). `Make*Provider` in each signal's own
/// header remain available for callers who want just one signal or want to
/// hold the `nostd::shared_ptr` themselves.

namespace microtel::adapters::otelcpp
{

/// @brief Register @p provider as the global trace, metrics, and logs
///        provider for otel-cpp's API.
///
/// Equivalent to calling `trace::Provider::SetTracerProvider`,
/// `metrics::Provider::SetMeterProvider`, and
/// `logs::Provider::SetLoggerProvider` with `MakeTracerProvider(provider)`,
/// `MakeMeterProvider(provider)`, and `MakeLoggerProvider(provider)`
/// respectively. Call once at process startup, after building @p provider
/// (e.g. via `SdkBuilder::Build()`).
///
/// @param provider the microtel provider to register. Must be non-null.
inline void RegisterGlobally(std::shared_ptr<microtel::Provider> provider)
{
    opentelemetry::trace::Provider::SetTracerProvider(MakeTracerProvider(provider));
    opentelemetry::metrics::Provider::SetMeterProvider(MakeMeterProvider(provider));
    opentelemetry::logs::Provider::SetLoggerProvider(MakeLoggerProvider(std::move(provider)));
}

/// @brief Restore otel-cpp's noop providers for all three signals.
///
/// Intended for tests: global provider state is a process-wide singleton,
/// so a test that calls `RegisterGlobally` must restore the noop default
/// afterwards or leak state into unrelated tests. Production code has no
/// occasion to call this — providers are registered once at startup and
/// live for the process.
inline void UnregisterGlobally() noexcept
{
    opentelemetry::trace::Provider::SetTracerProvider(
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>{
            std::make_shared<opentelemetry::trace::NoopTracerProvider>()});
    opentelemetry::metrics::Provider::SetMeterProvider(
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>{
            std::make_shared<opentelemetry::metrics::NoopMeterProvider>()});
    opentelemetry::logs::Provider::SetLoggerProvider(
        opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>{
            std::make_shared<opentelemetry::logs::NoopLoggerProvider>()});
}

}  // namespace microtel::adapters::otelcpp
