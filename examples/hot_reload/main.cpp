// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// hot_reload — retune a live pipeline with the four ICP 0026 `Provider`
// setters, without restarting the process.
//
// A long-running emitter: one root span every 50 ms for about a minute, with a
// schedule of configuration changes applied to the running provider. Every
// change prints the `Status` it returned, so the run is a transcript of what
// the setters do — including the one that is *rejected*.
//
// Usage:
//   hot_reload [endpoint]
//
// where [endpoint] defaults to http://localhost:4317 — the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// What to watch while it runs is in README.md: the span rate in Grafana halves
// ten times over when the sampler ratio drops from 1.0 to 0.1, roughly ten
// seconds in.

#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kSpanInterval{50};
constexpr std::chrono::seconds kRunTime{60};
constexpr std::chrono::seconds kReportInterval{5};
constexpr std::chrono::seconds kFlushTimeout{10};
constexpr std::chrono::seconds kShutdownTimeout{5};

constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr const char* kServiceName{"microtel-hot-reload-example"};
constexpr const char* kScopeVersion{"1.0.0"};

// The sampler the provider starts with. A ratio sampler, because
// `SetSamplerRatio` retunes a ratio in place and never converts a sampler of
// another kind into one that has a ratio — see README.md.
constexpr double kInitialRatio{1.0};
constexpr double kReducedRatio{0.1};

const char* LogLevelToString(microtel::LogLevel level) noexcept
{
    switch (level)
    {
        case microtel::LogLevel::Trace:
            return "trace";
        case microtel::LogLevel::Debug:
            return "debug";
        case microtel::LogLevel::Info:
            return "info";
        case microtel::LogLevel::Warn:
            return "warn";
        case microtel::LogLevel::Error:
            return "error";
    }
    return "?";
}

// Route microtel's internal diagnostics into this program's own output, so the
// *reason* a setter rejected a value appears next to the `Status` it returned.
// That reason is only ever in the internal log: `Status` says "InvalidArgument"
// and the log says which field and what range (ICP 0026 §2).
//
// The sink may be called from any microtel thread, so the line is assembled
// first and written in one `<<`: two threads logging at once then interleave
// whole lines rather than fragments.
void InstallLogSink()
{
    microtel::SetLogSink(
        [](microtel::LogLevel level, std::string_view msg)
        {
            const std::string line = std::string{"         [microtel "} + LogLevelToString(level) +
                                     "] " + std::string{msg} + "\n";
            std::cout << line;
        });
}

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
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

// ── The schedule ───────────────────────────────────────────────────────────
//
// One function per change, so the main loop stays a loop. Each returns the
// `Status` the setter returned, unmodified.

microtel::Status StepReduceSamplerRatio(microtel::Provider& provider) noexcept
{
    return provider.SetSamplerRatio(kReducedRatio);
}

// Raise the floor of microtel's internal diagnostic log above `Warn`. Every
// production log site inside microtel emits at `Warn` today, so this is the
// direction in which the knob is *observable*: after it, a rejected setter
// still returns `InvalidArgument` but stops explaining itself.
microtel::Status StepQuietInternalLogs(microtel::Provider& provider) noexcept
{
    return provider.SetLogLevel(microtel::LogLevel::Error);
}

microtel::Status StepRestoreInternalLogs(microtel::Provider& provider) noexcept
{
    return provider.SetLogLevel(microtel::LogLevel::Warn);
}

// Deliberately invalid: a zero `max_queue_size` makes a queue that can hold
// nothing, and `max_export_batch_size` above it can never be reached. The
// setter validates first and changes *nothing* — not the queue size, not the
// delay, not the half of the options that were fine.
microtel::Status StepRejectedBatchOptions(microtel::Provider& provider) noexcept
{
    return provider.SetBatchOptions({.max_queue_size = 0,
                                     .max_export_batch_size = 512,
                                     .schedule_delay = std::chrono::seconds{1},
                                     .drop_policy = microtel::DropPolicy::DropNewest});
}

// The same intent, expressed in values that pass validation: drain sooner and
// in smaller batches, which is what an operator reaches for when a collector
// is close to its limits.
microtel::Status StepValidBatchOptions(microtel::Provider& provider) noexcept
{
    return provider.SetBatchOptions({.max_queue_size = 4096,
                                     .max_export_batch_size = 128,
                                     .schedule_delay = std::chrono::seconds{1},
                                     .drop_policy = microtel::DropPolicy::DropNewest});
}

struct ReloadStep
{
    std::chrono::seconds at;
    const char* what;
    microtel::Status (*apply)(microtel::Provider&) noexcept;
};

// Reject loudly, go quiet, reject quietly, restore, accept. The two identical
// rejections either side of the log-level change are the point: the `Status`
// is the same both times and the explanation is not.
constexpr std::array<ReloadStep, 6> kSchedule{{
    {std::chrono::seconds{10}, "SetSamplerRatio(0.1)", &StepReduceSamplerRatio},
    {std::chrono::seconds{20},
     "SetBatchOptions({max_queue_size=0}) [invalid]",
     &StepRejectedBatchOptions},
    {std::chrono::seconds{30}, "SetLogLevel(Error)", &StepQuietInternalLogs},
    {std::chrono::seconds{40},
     "SetBatchOptions({max_queue_size=0}) [invalid, and now unexplained]",
     &StepRejectedBatchOptions},
    {std::chrono::seconds{45}, "SetLogLevel(Warn)", &StepRestoreInternalLogs},
    {std::chrono::seconds{50}, "SetBatchOptions({4096, 128, 1s}) [valid]", &StepValidBatchOptions},
}};

// ── The emitter ────────────────────────────────────────────────────────────

struct Counters
{
    std::uint64_t emitted = 0;
    std::uint64_t sampled = 0;
    std::uint64_t emitted_at_last_report = 0;
    std::uint64_t sampled_at_last_report = 0;
    std::string last_sampled_trace_id;
};

// One root span per tick. A root span is one trace, so the tick rate and the
// trace rate are the same number — which is what makes the sampler change
// visible in Grafana rather than merely plausible.
void EmitTick(microtel::Tracer& tracer, std::uint64_t index, Counters& counters)
{
    const auto span = tracer.StartSpan(
        "hot_reload.tick",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("tick.index", static_cast<std::int64_t>(index));
    span->SetStatus(microtel::StatusCode::Ok);

    const microtel::SpanContext ctx = span->GetContext();
    span->End();

    ++counters.emitted;
    if (ctx.trace_flags.IsSampled())
    {
        ++counters.sampled;
        counters.last_sampled_trace_id = ctx.trace_id.ToHex();
    }
}

void PrintReport(std::chrono::seconds elapsed, Counters& counters)
{
    const std::uint64_t emitted_delta = counters.emitted - counters.emitted_at_last_report;
    const std::uint64_t sampled_delta = counters.sampled - counters.sampled_at_last_report;
    counters.emitted_at_last_report = counters.emitted;
    counters.sampled_at_last_report = counters.sampled;

    std::cout << "t=+" << elapsed.count() << "s  emitted=" << counters.emitted
              << " sampled=" << counters.sampled << "  (last " << kReportInterval.count()
              << "s: " << emitted_delta << " emitted, " << sampled_delta << " sampled)";
    if (!counters.last_sampled_trace_id.empty())
    {
        std::cout << "  trace_id=" << counters.last_sampled_trace_id;
    }
    std::cout << '\n';
}

// Applies every scheduled change whose time has come. Returns the index of the
// first step still in the future.
std::size_t ApplyDueSteps(microtel::Provider& provider,
                          std::chrono::seconds elapsed,
                          std::size_t next_step)
{
    while (next_step < kSchedule.size() && elapsed >= kSchedule[next_step].at)
    {
        const ReloadStep& step = kSchedule[next_step];
        const microtel::Status status = step.apply(provider);
        std::cout << "t=+" << elapsed.count() << "s  " << step.what << " -> "
                  << StatusToString(status) << '\n';
        ++next_step;
    }
    return next_step;
}

void RunEmitter(microtel::Provider& provider, microtel::Tracer& tracer)
{
    const auto start = Clock::now();
    Counters counters;
    std::size_t next_step = 0;
    std::chrono::seconds next_report = kReportInterval;

    for (std::uint64_t tick = 0;; ++tick)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start);
        if (elapsed >= kRunTime)
        {
            break;
        }

        EmitTick(tracer, tick, counters);
        next_step = ApplyDueSteps(provider, elapsed, next_step);

        if (elapsed >= next_report)
        {
            PrintReport(elapsed, counters);
            next_report += kReportInterval;
        }
        std::this_thread::sleep_for(kSpanInterval);
    }

    PrintReport(kRunTime, counters);
    if (!counters.last_sampled_trace_id.empty())
    {
        std::cout << "\nlast sampled trace_id: " << counters.last_sampled_trace_id
                  << "\n  Grafana: http://localhost:3000 (Explore -> Tempo, or the "
                     "\"microtel - recent traces\" dashboard)\n";
    }
}

// ── The two outcomes the happy path never reaches ──────────────────────────

// `SetSamplerRatio` never converts a sampler of another kind into one that has
// a ratio: an `AlwaysOn` sampler has no ratio, so the call is refused and
// nothing changes. Shown with a throwaway provider because the demonstration
// is the *refusal* — there is no way to reach it from a ratio sampler.
void ShowUnsupportedOnAlwaysOnSampler(const std::string& endpoint)
{
    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName(kServiceName)
                     .WithProfileName("hot-reload-alwayson")
                     .WithSampler(microtel::MakeAlwaysOnSampler())
                     .Build();
    if (!built)
    {
        std::cerr << "warning: could not build the AlwaysOn provider: " << built.error().message
                  << '\n';
        return;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    // The call is made before the line is printed, not inside it: the setter
    // logs its reason through the sink, and a sink writing in the middle of an
    // unfinished `<<` chain would split this line in half.
    const microtel::Status refused = provider->SetSamplerRatio(kReducedRatio);
    std::cout << "SetSamplerRatio(0.5) on an AlwaysOn sampler -> " << StatusToString(refused)
              << '\n';
    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "  (that provider's Shutdown: " << StatusToString(shutdown) << ")\n";
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};

    // Before Build(), so the builder's own validation warnings land here too.
    InstallLogSink();

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName(kServiceName)
                     .WithServiceVersion(kScopeVersion)
                     // A ratio sampler at 1.0: everything is sampled until the
                     // schedule retunes it. `SetSamplerRatio` needs a sampler
                     // that *has* a ratio — the default is AlwaysOn, which
                     // does not.
                     .WithSampler(microtel::MakeTraceIdRatioSampler(kInitialRatio))
                     .Build();

    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "warning: Connect() to " << endpoint
                  << " failed: " << connected.error().message
                  << "\n         is an OTLP/gRPC collector listening there? "
                     "continuing — export is retried on flush.\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-hot-reload-example", kScopeVersion);

    std::cout << "endpoint: " << endpoint << "\nsampler:  TraceIdRatio(" << kInitialRatio
              << ")\nrunning for " << kRunTime.count() << "s, one root span every "
              << kSpanInterval.count() << "ms\n\n";

    RunEmitter(*provider, *tracer);

    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "\nForceFlush: " << StatusToString(flush) << '\n';

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " queue_depth=" << health.queue_depth_now << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "last_error: " << health.last_error_message << '\n';
    }

    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "Shutdown: " << StatusToString(shutdown) << '\n';

    // The remaining two outcomes of the setter contract, both after the work
    // is done so they cannot disturb it.
    const microtel::Status after_shutdown = provider->SetSamplerRatio(kReducedRatio);
    std::cout << "\nafter Shutdown, SetSamplerRatio(0.1) -> " << StatusToString(after_shutdown)
              << '\n';
    ShowUnsupportedOnAlwaysOnSampler(endpoint);

    return (flush == microtel::Status::Completed) ? 0 : 2;
}
