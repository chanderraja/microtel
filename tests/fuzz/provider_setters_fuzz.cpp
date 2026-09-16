// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the four hot-reload `Provider` setters — ICP 0026.
//
// Required by the v1.1 ships-when gate clause 2 (ICP 0024): the setters' input
// validation is "fuzz-tested by a harness driving randomized values and
// interleavings against a live provider in the standing fuzz job".
//
// The validation surface is small but real, and clause 2 names all three parts
// of it: value ranges (a NaN or out-of-range ratio, a non-positive interval or
// delay), batch-size / queue-depth coherence, and a ratio setter called on a
// sampler that is not a `TraceIdRatio`. What makes it worth fuzzing is not the
// arithmetic — it is that a rejected call must leave the provider *unchanged*
// and still usable, and that any ordering of accepted and rejected calls must
// leave it that way too. So the input is not one value; it is a **program**:
// the first byte picks the sampler shape, and the rest is a stream of opcodes
// the harness replays against one live provider.
//
// Three invariants are asserted outright rather than only looking for crashes:
//
//   1. **The verdict is a function of the value, not of history.** For every
//      call the harness computes independently whether the argument is valid,
//      and requires the returned `Status` to agree: `InvalidArgument` exactly
//      when the value is bad, never for a good one.
//   2. **Nothing else can come back.** A live provider's setter answers
//      `Completed`, `InvalidArgument` or `Unsupported` and nothing else — in
//      particular never `AlreadyShutDown` before `Shutdown`, and never
//      `Failed`, whose Doxygen ("an unrecoverable internal error") is the
//      wrong word for either rejection (ICP 0026 Rationale).
//   3. **`Unsupported` is a property of the pipeline, not of the value.** The
//      shape of the provider is fixed at the top of each run, so whether a
//      setter is supported cannot change between opcodes. A run that ever sees
//      both `Completed` and `Unsupported` out of the same setter has found a
//      state-dependence the design says is not there.
//
// After the program runs, the provider is shut down and every setter is
// required to answer `AlreadyShutDown` — the fork-safety read ordering (ICP
// 0026 §1) checked from the outside.
//
// Repro:
//   ./build-fuzz/tests/fuzz/provider_setters_fuzz <crash_file>

#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include "common/internal_log.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtmk = microtel::testing;

namespace
{

void Require(bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

/// A byte stream the opcode loop reads from. Runs dry rather than wrapping, so
/// a short input is a short program and the mutator controls the length.
class Cursor
{
public:
    Cursor(const std::uint8_t* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool Empty() const noexcept
    {
        return m_pos >= m_size;
    }

    [[nodiscard]] std::uint8_t Byte() noexcept
    {
        if (m_pos >= m_size)
        {
            return 0U;
        }
        const std::uint8_t value =
            m_data[m_pos];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ++m_pos;
        return value;
    }

    /// Fills @p out with raw bytes, zero-padding a short tail. Raw rather than
    /// derived from a range so the mutator can reach NaN, the infinities, and
    /// the subnormals — which is the point of fuzzing a `double` argument.
    void Raw(void* out, std::size_t count) noexcept
    {
        std::memset(out, 0, count);
        const std::size_t available = (m_pos < m_size) ? (m_size - m_pos) : 0U;
        const std::size_t take = (count < available) ? count : available;
        if (take > 0U)
        {
            std::memcpy(out,
                        m_data + m_pos,
                        take);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
        m_pos += take;
    }

private:
    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos{0};
};

/// What the provider was built with. Fixed for a run, so `Unsupported` is too.
struct Shape
{
    bool batching_spans = true;
    bool metrics = true;
    bool ratio_sampler = true;
};

/// Tracks the two answers a supported-or-not setter may give, so invariant 3
/// can be checked at the end of the run.
struct Verdicts
{
    bool saw_completed = false;
    bool saw_unsupported = false;

    void Record(mt::Status status) noexcept
    {
        saw_completed = saw_completed || (status == mt::Status::Completed);
        saw_unsupported = saw_unsupported || (status == mt::Status::Unsupported);
    }

    void RequireStable() const
    {
        Require(!(saw_completed && saw_unsupported));
    }
};

mt::BatchOptions BaseOpts() noexcept
{
    mt::BatchOptions opts;
    opts.max_queue_size = 4096;
    opts.max_export_batch_size = 512;
    opts.schedule_delay = std::chrono::hours(1);
    return opts;
}

mt::SamplerHandle MakeSampler(std::uint8_t selector)
{
    switch (selector % 5U)
    {
        case 0U:
            return mt::MakeTraceIdRatioSampler(0.5);
        case 1U:
            return mt::MakeParentBasedSampler(mt::MakeTraceIdRatioSampler(0.5));
        case 2U:
            return mt::MakeAlwaysOnSampler();
        case 3U:
            return mt::MakeAlwaysOffSampler();
        default:
            return mt::MakeParentBasedSampler(mt::MakeAlwaysOnSampler());
    }
}

[[nodiscard]] bool SamplerHasRatio(std::uint8_t selector) noexcept
{
    return (selector % 5U) <= 1U;
}

std::unique_ptr<mts::SdkProvider> BuildProvider(std::uint8_t selector, Shape& shape)
{
    shape.batching_spans = (selector & 0x20U) == 0U;
    shape.metrics = (selector & 0x40U) == 0U;
    shape.ratio_sampler = SamplerHasRatio(selector);

    auto exporter = std::make_unique<mtmk::MockExporter>();
    auto* const exporter_ptr = exporter.get();

    std::unique_ptr<mt::internal::ISpanProcessor> processor;
    mts::BatchSpanProcessor* bsp = nullptr;
    if (shape.batching_spans)
    {
        auto batching = std::make_unique<mts::BatchSpanProcessor>(
            exporter_ptr, std::make_shared<const mt::Resource>(), BaseOpts());
        bsp = batching.get();
        processor = std::move(batching);
    }
    else
    {
        processor = std::make_unique<mtmk::MockSpanProcessor>();
    }

    std::unique_ptr<mt::internal::IMetricExporter> metric_exporter;
    if (shape.metrics)
    {
        metric_exporter = std::make_unique<mtmk::MockMetricExporter>();
    }

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtmk::MockTransport>(),
        .codec = nullptr,
        .exporter = std::move(exporter),
        .batch_span_processor = bsp,
        .processor = std::move(processor),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = MakeSampler(selector),
        .span_limits = {},
        .connect_opts = {},
        .metric_exporter = std::move(metric_exporter),
        .metric_interval = std::chrono::hours(1),
        .log_exporter = std::make_unique<mtmk::MockLogExporter>(),
        .log_batch_opts = BaseOpts(),
    });
}

/// The harness's own copy of ICP 0026 §3's validation table. Written out
/// independently of the implementation on purpose: agreeing with the
/// production predicate by construction would test nothing.
[[nodiscard]] bool BatchOptionsAreValid(const mt::BatchOptions& opts) noexcept
{
    return opts.max_queue_size != 0U && opts.max_export_batch_size != 0U &&
           opts.max_export_batch_size <= opts.max_queue_size && opts.schedule_delay.count() > 0;
}

/// A live setter answers one of exactly three things, and `InvalidArgument`
/// exactly when the harness independently judged the value bad.
void RequireVerdict(mt::Status status, bool value_is_valid)
{
    Require(status == mt::Status::Completed || status == mt::Status::InvalidArgument ||
            status == mt::Status::Unsupported);
    Require((status == mt::Status::InvalidArgument) == !value_is_valid);
}

void DriveBatchOptions(mt::Provider& provider, Cursor& cursor, Verdicts& verdicts)
{
    mt::BatchOptions opts;
    std::uint32_t queue = 0;
    std::uint32_t batch = 0;
    std::int32_t delay_ms = 0;
    cursor.Raw(&queue, sizeof(queue));
    cursor.Raw(&batch, sizeof(batch));
    cursor.Raw(&delay_ms, sizeof(delay_ms));
    opts.max_queue_size = queue;
    opts.max_export_batch_size = batch;
    opts.schedule_delay = std::chrono::milliseconds{delay_ms};
    opts.drop_policy =
        ((cursor.Byte() & 1U) != 0U) ? mt::DropPolicy::DropOldest : mt::DropPolicy::DropNewest;

    const mt::Status status = provider.SetBatchOptions(opts);
    RequireVerdict(status, BatchOptionsAreValid(opts));
    verdicts.Record(status);
}

void DriveMetricInterval(mt::Provider& provider, Cursor& cursor, Verdicts& verdicts)
{
    std::int64_t raw = 0;
    cursor.Raw(&raw, sizeof(raw));
    const std::chrono::milliseconds interval{raw};

    const mt::Status status = provider.SetMetricInterval(interval);
    RequireVerdict(status, interval.count() > 0);
    verdicts.Record(status);
}

void DriveSamplerRatio(mt::Provider& provider, Cursor& cursor, Verdicts& verdicts)
{
    double ratio = 0.0;
    cursor.Raw(&ratio, sizeof(ratio));

    const bool valid = !std::isnan(ratio) && ratio >= 0.0 && ratio <= 1.0;
    const mt::Status status = provider.SetSamplerRatio(ratio);
    RequireVerdict(status, valid);
    verdicts.Record(status);
}

void DriveLogLevel(mt::Provider& provider, Cursor& cursor, Verdicts& verdicts)
{
    const std::uint8_t raw = cursor.Byte();
    const auto level = static_cast<mt::LogLevel>(raw);

    const mt::Status status = provider.SetLogLevel(level);
    RequireVerdict(status, raw <= static_cast<std::uint8_t>(mt::LogLevel::Error));
    verdicts.Record(status);
}

/// After `Shutdown`, every setter reads `m_shut_down` before any mutex and
/// answers `AlreadyShutDown` — whatever the argument (ICP 0026 §1).
void RequireShutDownAnswers(mt::Provider& provider)
{
    Require(provider.SetBatchOptions(BaseOpts()) == mt::Status::AlreadyShutDown);
    Require(provider.SetMetricInterval(std::chrono::milliseconds{0}) ==
            mt::Status::AlreadyShutDown);
    Require(provider.SetSamplerRatio(std::nan("")) == mt::Status::AlreadyShutDown);
    Require(provider.SetLogLevel(static_cast<mt::LogLevel>(200)) == mt::Status::AlreadyShutDown);
}

/// Bounded so a pathological input cannot turn one exec into a long run.
constexpr int kMaxOpcodes = 64;

/// Swallow the internal log once, at the first exec.
///
/// Every rejected setter call logs at `Warn`, and the harness exists to make
/// rejections happen — so without a sink every exec would write several lines
/// to stderr and the fuzzer would spend its time in `fprintf`. Raising the
/// minimum level instead would not work: `SetLogLevel` is one of the four
/// knobs under test and lowers it right back.
void InstallSilentSinkOnce()
{
    static const bool once = []
    {
        microtel::SetLogSink([](mt::LogLevel, std::string_view) {});
        return true;
    }();
    (void)once;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0U)
    {
        return 0;
    }
    InstallSilentSinkOnce();

    Cursor cursor{data, size};
    Shape shape;
    const std::uint8_t selector = cursor.Byte();
    const std::unique_ptr<mts::SdkProvider> provider = BuildProvider(selector, shape);

    Verdicts batch;
    Verdicts interval;
    Verdicts ratio;
    Verdicts level;

    for (int executed = 0; executed < kMaxOpcodes && !cursor.Empty(); ++executed)
    {
        switch (cursor.Byte() & 0x03U)
        {
            case 0U:
                DriveBatchOptions(*provider, cursor, batch);
                break;
            case 1U:
                DriveMetricInterval(*provider, cursor, interval);
                break;
            case 2U:
                DriveSamplerRatio(*provider, cursor, ratio);
                break;
            default:
                DriveLogLevel(*provider, cursor, level);
                break;
        }
    }

    // Invariant 3: support is a property of the provider's shape, which is
    // fixed, so no setter may have answered both ways in one run.
    batch.RequireStable();
    interval.RequireStable();
    ratio.RequireStable();
    level.RequireStable();

    // And the shape decided at build time is the shape that was observed.
    Require(!(batch.saw_unsupported && shape.batching_spans));
    Require(!(interval.saw_unsupported && shape.metrics));
    Require(!(ratio.saw_unsupported && shape.ratio_sampler));
    // `SetLogLevel` is always supported: the knob is process-global.
    Require(!level.saw_unsupported);

    Require(provider->Shutdown(std::chrono::milliseconds{500}) != mt::Status::AlreadyShutDown);
    RequireShutDownAnswers(*provider);

    // The level is process-global, so leave the shipped default behind rather
    // than letting one exec's last opcode colour the next one.
    Require(mt::internal::SetMinLogLevel(mt::LogLevel::Info));
    return 0;
}
