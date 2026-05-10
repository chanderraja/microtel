// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_tracer.hpp"

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

}  // namespace

namespace microtel::sdk
{

SdkTracer::SdkTracer(internal::ISampler* sampler,
                     internal::ISpanProcessor* processor,
                     std::shared_ptr<const Resource> resource,
                     internal::InstrumentationScope scope,
                     SpanLimitOptions limits) noexcept
    : m_sampler(sampler),
      m_processor(processor),
      m_resource(std::move(resource)),
      m_scope(std::move(scope)),
      m_limits(limits)
{
}

SpanHandle SdkTracer::StartSpan(std::string_view name, const StartSpanOptions& opts) noexcept
{
    // Resolve parent context.
    SpanContext parent_ctx;
    if (opts.parent.has_value())
    {
        parent_ctx = *opts.parent;
    }

    // Generate IDs.  Inherit TraceId from valid parent; generate new one for roots.
    const TraceId trace_id = parent_ctx.IsValid() ? parent_ctx.trace_id : GenerateTraceId();
    const SpanId span_id = GenerateSpanId();

    SpanContext ctx{
        .trace_id = trace_id,
        .span_id = span_id,
        .trace_flags =
            parent_ctx.IsValid() ? parent_ctx.trace_flags : TraceFlags{TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };

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

    if (result.decision == internal::SamplingDecision::Drop)
    {
        return MakeNoopHandle();
    }

    // Sampled or RecordOnly — heap-allocate the span.
    // Set sampled flag on the context when decision is RecordAndSample.
    if (result.decision == internal::SamplingDecision::RecordAndSample)
    {
        ctx.trace_flags = TraceFlags{TraceFlags::kSampled};
    }
    else
    {
        ctx.trace_flags = TraceFlags{0};
    }

    auto* raw = new (std::nothrow) SdkSpan(ctx,
                                           parent_ctx,
                                           name,
                                           opts.kind,
                                           opts.start_time,
                                           m_processor,
                                           m_resource,
                                           m_scope,
                                           m_limits);
    if (raw == nullptr)
    {
        return MakeNoopHandle();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — intentional: this IS the owning deleter
    SpanHandle handle{raw, internal::SpanDeleter{[](Span* s) noexcept { delete s; }}};

    // Call OnStart with an empty (default) parent context wrapper.
    Context parent_propagation_ctx{parent_ctx};
    m_processor->OnStart(*raw, parent_propagation_ctx);

    return handle;
}

SpanHandle SdkTracer::StartAsCurrentSpan(std::string_view name,
                                         const StartSpanOptions& opts) noexcept
{
    // Thread-local context propagation machinery deferred to v1.1.
    // For now behaves identically to StartSpan.
    return StartSpan(name, opts);
}

}  // namespace microtel::sdk
