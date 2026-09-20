// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// sampler_chains — composing head samplers into a rule chain.
//
// Phase 1 builds a first-match chain:
//
//   1. attribute rule   tenant == "premium"  -> AlwaysOn
//   2. span-kind rule   kind == Server       -> TraceIdRatio(0.5)
//   3. default                               -> AlwaysOff
//
// and emits a labelled batch of spans across all three cases, printing how
// many of each the chain sampled. Phase 2 repeats a two-rule chain under
// ChainMode::AllMustAgree, where every rule votes and one "no" drops the span.
//
// Usage:
//   sampler_chains [endpoint]
//
// where [endpoint] defaults to http://localhost:4317 — the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// Only sampled spans are exported, so the counts printed here are exactly what
// arrives in Tempo. See README.md for the Grafana queries that show it.

#include "microtel/provider.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr const char* kServiceName{"microtel-sampler-chains-example"};

/// Ratio the span-kind rule delegates to. Deterministic per trace ID, so the
/// count below lands near half and is not exactly half.
constexpr double kServerRatio{0.5};

/// Enough spans for a 50% ratio to read as a ratio rather than a coin flip.
constexpr std::size_t kSpansPerCase{12};

/// One span is all an all-must-agree case needs: it is a yes/no demonstration.
constexpr std::size_t kSpansPerAgreeCase{1};

/// Trace IDs printed per case. All of them would be 24 lines of hex; a few are
/// enough to paste into Tempo.
constexpr std::size_t kMaxPrintedTraceIds{2};

const char* StatusToString(microtel::Status status) noexcept
{
    switch (status)
    {
        case microtel::Status::Completed:
            return "Completed";
        case microtel::Status::TimedOut:
            return "TimedOut";
        case microtel::Status::AlreadyShutDown:
            return "AlreadyShutDown";
        case microtel::Status::Failed:
            return "Failed";
        // Setter-only outcomes. ForceFlush and Shutdown never return these,
        // but the switch is exhaustive so -Wswitch keeps this honest if the
        // enum grows again.
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

const char* SpanKindName(microtel::SpanKind kind) noexcept
{
    switch (kind)
    {
        case microtel::SpanKind::Internal:
            return "Internal";
        case microtel::SpanKind::Server:
            return "Server";
        case microtel::SpanKind::Client:
            return "Client";
        case microtel::SpanKind::Producer:
            return "Producer";
        case microtel::SpanKind::Consumer:
            return "Consumer";
    }
    return "Unknown";
}

/// @brief The first-match chain described at the top of the file.
///
/// Note `std::string{"premium"}` rather than the bare literal: `AttributeValue`
/// is a `std::variant` whose first alternative is `bool`, and a `const char*`
/// binds to it — the rule would then be comparing against `true` and never
/// match.
microtel::SamplerHandle MakeFirstMatchChain()
{
    return microtel::MakeChainSampler(
        microtel::ChainMode::FirstMatch,
        microtel::MakeAttributeRuleSampler("tenant",
                                           std::string{"premium"},
                                           microtel::MakeAlwaysOnSampler(),
                                           microtel::MakeAlwaysOffSampler()),
        microtel::MakeSpanKindRuleSampler(microtel::SpanKind::Server,
                                          microtel::MakeTraceIdRatioSampler(kServerRatio),
                                          microtel::MakeAlwaysOffSampler()),
        microtel::MakeAlwaysOffSampler());
}

/// @brief Two rules that must both say yes.
///
/// Each rule's no-match arm is `AlwaysOff`, and under `AllMustAgree` that arm
/// is what a miss answers through — which is the whole difference from the
/// chain above, where a rule that misses is skipped and the next rule decides.
microtel::SamplerHandle MakeAllMustAgreeChain()
{
    return microtel::MakeChainSampler(
        microtel::ChainMode::AllMustAgree,
        microtel::MakeAttributeRuleSampler("tenant",
                                           std::string{"premium"},
                                           microtel::MakeAlwaysOnSampler(),
                                           microtel::MakeAlwaysOffSampler()),
        microtel::MakeSpanKindRuleSampler(microtel::SpanKind::Server,
                                          microtel::MakeAlwaysOnSampler(),
                                          microtel::MakeAlwaysOffSampler()));
}

/// @brief One row of the demonstration: a span name, a kind, and a tenant.
struct SamplerCase
{
    std::string_view span_name;
    microtel::SpanKind kind;
    std::string_view tenant;
};

/// @brief Emit @p count spans for @p sample_case and report how many survived.
///
/// The tenant is passed as an **initial attribute**, not set after the fact:
/// `ShouldSample` runs inside `StartSpan` and sees only `StartSpanOptions`, so
/// an attribute applied with `SetAttribute` afterwards cannot influence the
/// decision — by then the decision is made.
void RunCase(microtel::Tracer& tracer, const SamplerCase& sample_case, std::size_t count)
{
    const std::vector<microtel::KeyValue> attributes{
        {.key = "tenant", .value = std::string{sample_case.tenant}}};

    std::size_t sampled = 0;
    std::vector<std::string> trace_ids;

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto span = tracer.StartSpan(
            sample_case.span_name,
            {.kind = sample_case.kind, .parent = {}, .start_time = {}, .attributes = attributes});
        if (span->IsSampled())
        {
            ++sampled;
            if (trace_ids.size() < kMaxPrintedTraceIds)
            {
                trace_ids.push_back(span->GetContext().trace_id.ToHex());
            }
        }
        span->End();
    }

    std::cout << "  " << sample_case.span_name << "  tenant=" << sample_case.tenant
              << " kind=" << SpanKindName(sample_case.kind) << "  ->  sampled " << sampled << '/'
              << count << '\n';
    for (const std::string& id : trace_ids)
    {
        std::cout << "      trace_id: " << id << '\n';
    }
}

/// @brief Build one provider carrying @p sampler under profile @p profile.
///
/// Two chains mean two providers: a provider's sampler is fixed for its life.
/// They differ only in profile name, which has to be unique among live
/// providers — see `examples/multi_profile/`.
microtel::Expected<std::shared_ptr<microtel::Provider>, microtel::ConfigError> BuildProvider(
    const std::string& endpoint, std::string profile, microtel::SamplerHandle sampler)
{
    return microtel::SdkBuilder{}
        .WithEndpoint(endpoint)
        .WithProtocol(microtel::Protocol::Grpc)
        .WithServiceName(kServiceName)
        .WithServiceVersion("1.0.0")
        .WithProfileName(std::move(profile))
        .WithSampler(std::move(sampler))
        .Build();
}

/// @brief Flush, report health, shut down. True if the flush completed.
bool Finish(microtel::Provider& provider)
{
    const microtel::Status flush = provider.ForceFlush(kFlushTimeout);
    const microtel::HealthSnapshot health = provider.GetExporterHealth();
    const microtel::Status shutdown = provider.Shutdown(kShutdownTimeout);

    std::cout << "  ForceFlush: " << StatusToString(flush)
              << "  batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " queue_depth=" << health.queue_depth_now
              << "  Shutdown: " << StatusToString(shutdown) << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "  last_error: " << health.last_error_message << '\n';
    }

    return flush == microtel::Status::Completed;
}

/// @brief Phase 1 — the first-match chain.
bool RunFirstMatchPhase(const std::string& endpoint)
{
    auto built = BuildProvider(endpoint, "first-match", MakeFirstMatchChain());
    if (!built)
    {
        std::cerr << "Build() failed: " << built.error().message << '\n';
        return false;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "warning: Connect() failed: " << connected.error().message
                  << " — continuing; export is retried on flush.\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer = provider->GetTracer("chain.demo", "1.0.0");

    std::cout << "\nfirst-match chain  [tenant=premium -> AlwaysOn | kind=Server -> ratio "
              << kServerRatio << " | default -> AlwaysOff]\n";

    // Client kind, so rule 1 is demonstrably the rule that decided: rule 2
    // would not have matched this span at all.
    RunCase(*tracer,
            {.span_name = "chain.premium.checkout",
             .kind = microtel::SpanKind::Client,
             .tenant = "premium"},
            kSpansPerCase);
    RunCase(
        *tracer,
        {.span_name = "chain.server.request", .kind = microtel::SpanKind::Server, .tenant = "free"},
        kSpansPerCase);
    RunCase(*tracer,
            {.span_name = "chain.background.sweep",
             .kind = microtel::SpanKind::Internal,
             .tenant = "free"},
            kSpansPerCase);

    return Finish(*provider);
}

/// @brief Phase 2 — the same two rules, under AllMustAgree.
bool RunAllMustAgreePhase(const std::string& endpoint)
{
    auto built = BuildProvider(endpoint, "all-must-agree", MakeAllMustAgreeChain());
    if (!built)
    {
        std::cerr << "Build() failed: " << built.error().message << '\n';
        return false;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "warning: Connect() failed: " << connected.error().message
                  << " — continuing; export is retried on flush.\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer = provider->GetTracer("chain.demo", "1.0.0");

    std::cout << "\nall-must-agree chain  [tenant=premium AND kind=Server]\n";

    RunCase(*tracer,
            {.span_name = "agree.premium.server",
             .kind = microtel::SpanKind::Server,
             .tenant = "premium"},
            kSpansPerAgreeCase);
    RunCase(*tracer,
            {.span_name = "agree.premium.internal",
             .kind = microtel::SpanKind::Internal,
             .tenant = "premium"},
            kSpansPerAgreeCase);
    RunCase(
        *tracer,
        {.span_name = "agree.free.server", .kind = microtel::SpanKind::Server, .tenant = "free"},
        kSpansPerAgreeCase);

    return Finish(*provider);
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};
    std::cout << "endpoint: " << endpoint << "\nservice.name: " << kServiceName << '\n';

    const bool first_match_ok = RunFirstMatchPhase(endpoint);
    const bool all_must_agree_ok = RunAllMustAgreePhase(endpoint);

    std::cout << "\nOnly the sampled spans were exported. In Grafana:\n"
                 "  { resource.service.name = \""
              << kServiceName << "\" }\n";

    return (first_match_ok && all_must_agree_ok) ? 0 : 2;
}
