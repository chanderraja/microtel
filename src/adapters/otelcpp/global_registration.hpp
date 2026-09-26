// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"

#include "adapters/otelcpp/logger_shim.hpp"
#include "adapters/otelcpp/meter_shim.hpp"
#include "adapters/otelcpp/shim_options.hpp"
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
///
/// @note **There is no prebuilt shim library, and there never will be — this
/// header and its siblings are compiled inside your build.** The
/// opentelemetry-cpp API puts `nostd::` types in every signature and wraps
/// everything in `inline namespace v<OPENTELEMETRY_ABI_VERSION_NO>`, so the
/// same shim source compiled under two configurations produces
/// link-incompatible symbols. Which configuration is right is a choice *you*
/// made when you built opentelemetry-cpp (`OPENTELEMETRY_STL_VERSION`,
/// `OPENTELEMETRY_ABI_VERSION_NO`), so a shipped archive would be wrong for
/// most consumers and silently so. `cmake --install` therefore places these
/// headers under `<includedir>/microtel-shim/` — add that directory to your
/// include path, compile the shim in your own tree against your own
/// opentelemetry-cpp, and link `microtel::microtel` alongside it. See
/// ICP 0014 §1 and ICP 0020 Decision 3.

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
/// @note **One profile, whichever you hand it.** This takes the provider to
///       bind as a parameter and consults no global of microtel's, so v1.1's
///       multi-profile registry changes nothing here: existing code passes the
///       provider it built, which is the default profile. otel-cpp's own API
///       holds exactly one provider per signal, so only the profile registered
///       here is reachable through instrumentation written against otel-cpp;
///       other profiles are reached through microtel's own API —
///       `microtel::GetProvider("name")` — and cannot be routed through
///       otel-cpp's globals at the same time. See
///       [ICP 0027](../../../docs/icps/0027-multi-profile-threading.md) §6.3.
///
/// @note **Calling it again.** Each call builds three new provider shims with
///       their own copy of @p options and replaces otel-cpp's globals, so the
///       last call wins for every tracer, meter and logger obtained through
///       the globals after it. Objects already handed out keep the options
///       they were created with, for their lifetime (ICP 0033 §6).
///
/// @param provider the microtel provider to register. Must be non-null.
/// @param options  passed to all three providers (ICP 0033). If you set a
///                 non-default `SpanLimitOptions::attribute_value_length_limit`,
///                 pass the same value here.
inline void RegisterGlobally(std::shared_ptr<microtel::Provider> provider, ShimOptions options = {})
{
    opentelemetry::trace::Provider::SetTracerProvider(MakeTracerProvider(provider, options));
    opentelemetry::metrics::Provider::SetMeterProvider(MakeMeterProvider(provider, options));
    opentelemetry::logs::Provider::SetLoggerProvider(
        MakeLoggerProvider(std::move(provider), options));
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
