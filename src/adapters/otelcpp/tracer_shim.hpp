// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"
#include "microtel/tracer.hpp"

#include "adapters/otelcpp/abi_guard.hpp"
#include "adapters/otelcpp/attribute_conversion.hpp"
#include "adapters/otelcpp/context_conversion.hpp"
#include "adapters/otelcpp/span_shim.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

#include <opentelemetry/context/context.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/span_context_kv_iterable.h>
#include <opentelemetry/trace/span_startoptions.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/trace/tracer_provider.h>

/// @file
/// `TracerShim` / `TracerProviderShim` — implement otel-cpp's `trace::Tracer`
/// and `trace::TracerProvider` (ABI v1) over `microtel::Tracer` and
/// `microtel::Provider`. Spans come back wrapped in `SpanShim`.

namespace microtel::adapters::otelcpp
{

namespace detail
{

/// @brief Map otel-cpp's span kind onto microtel's.
[[nodiscard]] constexpr microtel::SpanKind ToMicrotelSpanKind(
    opentelemetry::trace::SpanKind kind) noexcept
{
    switch (kind)
    {
        case opentelemetry::trace::SpanKind::kServer:
            return microtel::SpanKind::Server;
        case opentelemetry::trace::SpanKind::kClient:
            return microtel::SpanKind::Client;
        case opentelemetry::trace::SpanKind::kProducer:
            return microtel::SpanKind::Producer;
        case opentelemetry::trace::SpanKind::kConsumer:
            return microtel::SpanKind::Consumer;
        case opentelemetry::trace::SpanKind::kInternal:
            return microtel::SpanKind::Internal;
    }
    return microtel::SpanKind::Internal;
}

/// @brief The currently active otel span's context, if one is attached and
///        valid — the `trace::Scope` / `WithActiveSpan` thread-local.
[[nodiscard]] inline std::optional<microtel::SpanContext> ActiveSpanParent() noexcept
{
    const auto active =
        opentelemetry::trace::GetSpan(opentelemetry::context::RuntimeContext::GetCurrent());
    if (active != nullptr && active->GetContext().IsValid())
    {
        return ToMicrotelSpanContext(active->GetContext());
    }
    return std::nullopt;
}

/// @brief Resolve otel-cpp's parent variant into microtel's optional parent.
///
/// The three otel states map onto microtel's `StartSpanOptions::parent`:
///
/// - **valid `SpanContext`** → that context, converted.
/// - **invalid `SpanContext`** (otel's default) → the currently active span
///   (`trace::Scope`), if any; otherwise unset. microtel's SDK does not yet
///   consult a current context on its own (`StartAsCurrentSpan` is v1.1), so
///   the shim performs the inheritance otel-cpp semantics require.
/// - **`context::Context`** → the span it carries, if any; a context flagged
///   `is_root_span` becomes a *set but invalid* parent, which microtel's SDK
///   treats as an explicit root (fresh trace id); a context carrying neither
///   is unset.
[[nodiscard]] inline std::optional<microtel::SpanContext> ResolveParent(
    const opentelemetry::nostd::variant<opentelemetry::trace::SpanContext,
                                        opentelemetry::context::Context>& parent) noexcept
{
    if (const auto* span_context = std::get_if<opentelemetry::trace::SpanContext>(&parent))
    {
        if (span_context->IsValid())
        {
            return ToMicrotelSpanContext(*span_context);
        }
        return ActiveSpanParent();
    }

    const auto& context = std::get<opentelemetry::context::Context>(parent);
    if (opentelemetry::trace::IsRootSpan(context))
    {
        return microtel::SpanContext{};  // set-but-invalid: explicit root
    }
    const auto span = opentelemetry::trace::GetSpan(context);
    if (span != nullptr && span->GetContext().IsValid())
    {
        return ToMicrotelSpanContext(span->GetContext());
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief An otel-cpp tracer backed by a microtel tracer.
///
/// Holds the provider alongside the tracer because otel-cpp puts flush and
/// close on the tracer while microtel puts them on the provider.
/// `CloseWithMicroseconds` therefore shuts down the whole provider — the same
/// blast radius otel-cpp's own SDK has, where closing a tracer shuts down the
/// shared processor pipeline.
///
/// @threadsafety Thread-safe, matching `microtel::Tracer`.
class TracerShim final : public opentelemetry::trace::Tracer
{
public:
    /// @param tracer   the microtel tracer to start spans on. Must be non-null.
    /// @param provider the owning provider, for flush/close. Must be non-null.
    ///
    /// `shared_ptr` throughout is dictated by the otel-cpp API surface
    /// (`GetTracer`/`StartSpan` return `nostd::shared_ptr`) and matches
    /// microtel's own `Provider::GetTracer` joint-ownership contract.
    TracerShim(std::shared_ptr<microtel::Tracer> tracer,
               std::shared_ptr<microtel::Provider> provider) noexcept
        : m_tracer{std::move(tracer)}, m_provider{std::move(provider)}
    {
    }

    // Re-expose the base class's non-virtual StartSpan convenience overloads
    // the override below would otherwise hide.
    using opentelemetry::trace::Tracer::StartSpan;

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> StartSpan(
        opentelemetry::nostd::string_view name,
        const otel_common::KeyValueIterable& attributes,
        const opentelemetry::trace::SpanContextKeyValueIterable& links,
        const opentelemetry::trace::StartSpanOptions& options) noexcept override
    {
        const std::vector<microtel::KeyValue> initial_attributes = ConvertKeyValues(attributes);
        const microtel::StartSpanOptions start_options{
            .kind = detail::ToMicrotelSpanKind(options.kind),
            .parent = detail::ResolveParent(options.parent),
            .start_time =
                static_cast<std::chrono::system_clock::time_point>(options.start_system_time),
            .attributes = microtel::AttributeSpan{initial_attributes},
        };

        microtel::SpanHandle handle =
            m_tracer->StartSpan({name.data(), name.size()}, start_options);

        // microtel takes links post-creation; otel-cpp only at creation
        // (AddLink is ABI v2). Forward each before the span is handed out.
        links.ForEachKeyValue(
            [&handle](opentelemetry::trace::SpanContext link_context,
                      const otel_common::KeyValueIterable& link_attributes) noexcept
            {
                const std::vector<microtel::KeyValue> converted = ConvertKeyValues(link_attributes);
                handle->AddLink(ToMicrotelSpanContext(link_context),
                                microtel::AttributeSpan{converted});
                return true;
            });

        return opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>{
            std::make_shared<SpanShim>(std::move(handle))};
    }

    /// @brief Flush buffered spans; delegates to `Provider::ForceFlush`.
    void ForceFlushWithMicroseconds(std::uint64_t timeout) noexcept override
    {
        std::ignore = m_provider->ForceFlush(ToMilliseconds(timeout));
    }

    /// @brief Close: flushes and shuts down the provider. otel-cpp offers no
    /// status channel here, so the returned `Status` is dropped.
    void CloseWithMicroseconds(std::uint64_t timeout) noexcept override
    {
        std::ignore = m_provider->Shutdown(ToMilliseconds(timeout));
    }

private:
    [[nodiscard]] static std::chrono::milliseconds ToMilliseconds(
        std::uint64_t timeout_microseconds) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::microseconds{timeout_microseconds});
    }

    std::shared_ptr<microtel::Tracer> m_tracer;
    std::shared_ptr<microtel::Provider> m_provider;
};

/// @brief An otel-cpp tracer provider backed by a microtel provider.
///
/// `schema_url` has no surface on `microtel::Provider::GetTracer` and is
/// dropped; name and version pass through unchanged.
///
/// @threadsafety Thread-safe, matching `microtel::Provider`.
class TracerProviderShim final : public opentelemetry::trace::TracerProvider
{
public:
    /// @param provider the microtel provider to adapt. Must be non-null.
    explicit TracerProviderShim(std::shared_ptr<microtel::Provider> provider) noexcept
        : m_provider{std::move(provider)}
    {
    }

    // Default arguments mirror the base declaration so direct calls through
    // the shim type keep otel-cpp's one-argument form.
    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> GetTracer(
        opentelemetry::nostd::string_view name,
        opentelemetry::nostd::string_view version = "",
        opentelemetry::nostd::string_view /*schema_url*/ = "") noexcept override
    {
        return opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer>{
            std::make_shared<TracerShim>(
                m_provider->GetTracer({name.data(), name.size()}, {version.data(), version.size()}),
                m_provider)};
    }

private:
    std::shared_ptr<microtel::Provider> m_provider;
};

/// @brief Build an otel-cpp tracer provider over a microtel provider, ready
///        for `opentelemetry::trace::Provider::SetTracerProvider`.
///
/// The one-liner an adopting application calls at startup:
/// ```cpp
/// opentelemetry::trace::Provider::SetTracerProvider(
///     microtel::adapters::otelcpp::MakeTracerProvider(microtel_provider));
/// ```
/// After that, already-instrumented otel-cpp code routes to microtel with no
/// call-site edits — the point of the shim (ICP 0014).
[[nodiscard]] inline opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>
MakeTracerProvider(std::shared_ptr<microtel::Provider> provider)
{
    return opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>{
        std::make_shared<TracerProviderShim>(std::move(provider))};
}

}  // namespace microtel::adapters::otelcpp
