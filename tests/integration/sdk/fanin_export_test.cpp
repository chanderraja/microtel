// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Issue #345: the export side of a concentrator must hold as many spans while
// a request is in flight whether they come from one Resource or from many.
//
// The real BatchSpanProcessor hands each drain to the real OtlpExporter as one
// BatchHandle per (Resource, scope) group (ICP 0023, design §3.6.1). The
// exporter's queue used to be bounded only in BatchHandles, so a drain from
// 128 leaves took 128 of its 256 slots where a one-leaf drain took one: two
// drains filled it, and everything behind a slow request was dropped as
// `queue_full`. The schedule here is fixed by a gate, not by timing: the
// exporter's worker is held inside its first Send while the processor hands
// over every drain, so the test sees exactly what the queue refused.

#include "microtel/internal/batch.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include "exporter/otlp_exporter.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_wire_codec.hpp"
#include "mocks/mock_otlp_encoder.hpp"
#include "sdk/batch_span_processor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtm = microtel::testing;
namespace mts = microtel::sdk;

namespace
{

constexpr auto kTimeout = std::chrono::seconds(10);
/// Spans each simulated leaf contributes per turn of the round-robin.
constexpr std::size_t kSpansPerPayload = 4;
/// Enough to fill several processor batches: at 512 spans a batch this is
/// eight drains, each holding every leaf of the 128-leaf case.
constexpr std::size_t kTotalSpans = 4096;

/// A FakeWireCodec whose first Send blocks until the test releases it, so the
/// exporter's worker is busy with one request while the processor keeps
/// handing over drains.
class GatedCodec final : public mtm::FakeWireCodec
{
public:
    [[nodiscard]] mti::WireResult Send(mti::EncodedPayload&& payload,
                                       std::chrono::milliseconds deadline) override
    {
        {
            std::unique_lock lock{m_gate_mu};
            m_entered = true;
            m_gate_cv.notify_all();
            m_gate_cv.wait(lock, [this] { return m_released; });
        }
        return FakeWireCodec::Send(std::move(payload), deadline);
    }

    void WaitUntilEntered()
    {
        std::unique_lock lock{m_gate_mu};
        m_gate_cv.wait(lock, [this] { return m_entered; });
    }

    void Release()
    {
        const std::scoped_lock lock{m_gate_mu};
        m_released = true;
        m_gate_cv.notify_all();
    }

private:
    std::mutex m_gate_mu;
    std::condition_variable m_gate_cv;
    bool m_entered = false;
    bool m_released = false;
};

mti::SpanRecord Span(std::shared_ptr<const mt::Resource> resource)
{
    mti::SpanRecord r{.name = "leaf.span"};
    r.resource = std::move(resource);
    return r;
}

std::uint64_t Drops(const mtm::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

class FanInExportTest : public ::testing::TestWithParam<std::size_t>
{
};

}  // namespace

// With one leaf this passed before the fix; with 128 or 512 leaves the
// exporter refused every drain after its 256th BatchHandle.
TEST_P(FanInExportTest, ExporterQueueHoldsTheSameSpansWhateverTheLeafCount)
{
    const std::size_t leaves = GetParam();
    std::vector<std::shared_ptr<const mt::Resource>> resources;
    resources.reserve(leaves);
    for (std::size_t i = 0; i < leaves; ++i)
    {
        resources.push_back(std::make_shared<const mt::Resource>(std::vector<mt::KeyValue>{
            {.key = "leaf.index", .value = static_cast<std::int64_t>(i)}}));
    }

    mtm::MockOtlpEncoder encoder;
    GatedCodec codec;
    codec.default_result = mti::WireResult{.success = true};
    mtm::FakeDiagnosticsSink exporter_sink;
    mtm::FakeDiagnosticsSink processor_sink;
    // The exporter as SdkBuilder configures it for the default BatchOptions.
    mt::exporter::OtlpExporter exporter{
        &encoder, &codec, mt::exporter::OtlpExporterConfig{}, &exporter_sink};
    mts::BatchSpanProcessor processor{&exporter,
                                      std::make_shared<const mt::Resource>(),
                                      mt::BatchOptions{.schedule_delay = std::chrono::hours(1)},
                                      mt::MemoryLimitOptions{}.max_record_bytes,
                                      mt::MemoryLimitOptions{}.max_total_queue_bytes,
                                      &processor_sink,
                                      &exporter};
    const mti::InstrumentationScope scope{.name = "leaf-lib", .version = "1"};

    // Occupy the exporter's worker with one request it cannot finish yet.
    ASSERT_TRUE(processor.Enqueue(Span(resources.front()), scope));
    ASSERT_EQ(processor.ForceFlush(kTimeout), mt::Status::Completed);
    codec.WaitUntilEntered();

    // Round-robin over the leaves, kSpansPerPayload spans a turn, as the
    // leaf-fanin bench does. The processor drains every 512 spans; ForceFlush
    // returns once it has handed all of them to the (blocked) exporter.
    for (std::size_t i = 0; i < kTotalSpans; ++i)
    {
        ASSERT_TRUE(processor.Enqueue(Span(resources[(i / kSpansPerPayload) % leaves]), scope));
    }
    ASSERT_EQ(processor.ForceFlush(kTimeout), mt::Status::Completed);

    EXPECT_EQ(Drops(processor_sink, mt::DropReason::QueueFull), 0U);
    EXPECT_EQ(Drops(exporter_sink, mt::DropReason::QueueFull), 0U)
        << leaves << " leaves: the exporter refused spans that one leaf's drains would have fit";

    codec.Release();
    ASSERT_EQ(exporter.ForceFlush(kTimeout), mt::Status::Completed);
    // Every drain reached the codec; none of them was lost to the queue.
    EXPECT_EQ(exporter_sink.batches_failed, 0U);
    EXPECT_EQ(Drops(exporter_sink, mt::DropReason::QueueFull), 0U);

    ASSERT_EQ(processor.Shutdown(kTimeout), mt::Status::Completed);
    processor.JoinWorker();
}

INSTANTIATE_TEST_SUITE_P(Leaves,
                         FanInExportTest,
                         ::testing::Values(std::size_t{1}, std::size_t{128}, std::size_t{512}),
                         [](const ::testing::TestParamInfo<std::size_t>& info)
                         { return std::to_string(info.param) + "Leaves"; });
