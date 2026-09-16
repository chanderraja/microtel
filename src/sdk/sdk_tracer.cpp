// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_tracer.hpp"

#include "microtel/attribute.hpp"
#include "microtel/context.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include "sdk/noop_span.hpp"
#include "sdk/sdk_span.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <string_view>
#include <thread>
#include <utility>

namespace
{

// Thread-local RNG seeded from time + thread id to avoid contention and
// inter-thread correlation.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp,readability-identifier-naming)
thread_local std::mt19937_64 tl_rng{
    static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
    std::hash<std::thread::id>{}(std::this_thread::get_id())};

microtel::SpanId GenerateSpanId() noexcept
{
    const std::uint64_t v = tl_rng();
    microtel::SpanId::Bytes bytes{};
    std::memcpy(bytes.data(), &v, sizeof(v));
    return microtel::SpanId{bytes};
}

microtel::TraceId GenerateTraceId() noexcept
{
    const std::uint64_t hi = tl_rng();
    const std::uint64_t lo = tl_rng();
    microtel::TraceId::Bytes bytes{};
    std::memcpy(bytes.data(), &hi, sizeof(hi));
    std::memcpy(bytes.data() + sizeof(hi), &lo, sizeof(lo));
    return microtel::TraceId{bytes};
}

/// @brief Put `StartSpanOptions::attributes` on @p span (issue #265).
///
/// They go on through `SetAttribute` rather than straight into the record so
/// that one count budget, one value-length clip and one set of drop counters
/// cover initial and later attributes alike. Per attribute this costs exactly
/// what a `SetAttribute` call costs: the key copy and the value copy.
///
/// Called only on the sampled path, after the sampler's drop decision — the
/// unsampled path stays allocation-free (`docs/memory-model.md` §8.1).
void SeedInitialAttributes(microtel::Span& span, microtel::AttributeSpan attributes) noexcept
{
    for (const microtel::KeyValue& kv : attributes)
    {
        span.SetAttribute(kv.key, kv.value);
    }
}

}  // namespace

namespace microtel::sdk
{

SdkTracer::SdkTracer(internal::ISampler* sampler,
                     internal::ISpanProcessor* processor,
                     std::shared_ptr<const Resource> resource,
                     internal::InstrumentationScope scope,
                     SpanLimitOptions limits,
                     internal::IDiagnosticsSink* diagnostics) noexcept
    : m_sampler(sampler),
      m_processor(processor),
      m_resource(std::move(resource)),
      m_scope(std::move(scope)),
      m_limits(limits),
      m_diagnostics(diagnostics)
{
}

SpanHandle SdkTracer::StartSpanInternal(std::string_view name,
                                        const StartSpanOptions& opts,
                                        SpanContext* started) noexcept
{
    // Resolve the parent. An explicit parent always wins — including a
    // set-but-invalid one, which is how a caller asks for an explicit root.
    // Only an unset parent consults the calling thread's current context
    // (ICP 0025 §3 contract 1).
    const SpanContext parent_ctx =
        opts.parent.has_value() ? *opts.parent : CurrentContext().active_span_context;

    // Generate IDs.  Inherit TraceId from valid parent; generate new one for roots.
    const TraceId trace_id = parent_ctx.IsValid() ? parent_ctx.trace_id : GenerateTraceId();
    const SpanId span_id = GenerateSpanId();

    // Ask the sampler.
    const internal::SamplingContext sctx{
        .parent = parent_ctx,
        .span_kind = opts.kind,
        .span_name = name,
        .initial_attributes = opts.attributes,
        .links = {},
        .trace_id = trace_id,
    };
    const internal::SamplingResult result = m_sampler->ShouldSample(sctx);
    const bool sampled = result.decision == internal::SamplingDecision::RecordAndSample;

    const SpanContext ctx{
        .trace_id = trace_id,
        .span_id = span_id,
        .trace_flags = sampled ? TraceFlags{TraceFlags::kSampled} : TraceFlags{0},
        .trace_state = {},
        .remote = false,
    };

    // Published before the drop check: a dropped span still contributes a real
    // trace id and span id to the context its children inherit, which is what
    // keeps an unsampled subtree inside one trace (ICP 0025 §3 contract 3).
    if (started != nullptr)
    {
        *started = ctx;
    }

    if (result.decision == internal::SamplingDecision::Drop)
    {
        return MakeNoopHandle();
    }

    auto* raw = new (std::nothrow) SdkSpan(ctx,
                                           parent_ctx,
                                           name,
                                           opts.kind,
                                           opts.start_time,
                                           m_processor,
                                           m_resource,
                                           m_scope,
                                           m_limits,
                                           m_diagnostics);
    if (raw == nullptr)
    {
        return MakeNoopHandle();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — intentional: this IS the owning deleter
    SpanHandle handle{raw, internal::SpanDeleter{[](Span* s) noexcept { delete s; }}};
    SeedInitialAttributes(*raw, opts.attributes);

    // The Context handed to OnStart carries the resolved parent — explicit if
    // the caller supplied one, otherwise the thread's current span — and the
    // thread's baggage, which is per-context rather than per-span and so comes
    // from the current context however the parent was resolved (ICP 0025 §2
    // and §3 contract 6: baggage never parents). The copy is two refcount
    // bumps, and nothing here allocates.
    const Context parent_propagation_ctx{parent_ctx, CurrentContext().baggage};
    m_processor->OnStart(*raw, parent_propagation_ctx);

    return handle;
}

SpanHandle SdkTracer::StartSpan(std::string_view name, const StartSpanOptions& opts) noexcept
{
    return StartSpanInternal(name, opts, nullptr);
}

ScopedSpan SdkTracer::StartAsCurrentSpan(std::string_view name,
                                         const StartSpanOptions& opts) noexcept
{
    SpanContext started;
    SpanHandle handle = StartSpanInternal(name, opts, &started);
    return ScopedSpan{std::move(handle), Context{started}};
}

}  // namespace microtel::sdk
