// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 sugar layer — the one integration test (ICP 0028 §3).
//
// A call tree written entirely in `microtel::sugar` — `Span` with inline
// `AttrKey` attributes, `Traced`, `MICROTEL_TRACE_FUNCTION`, `TraceFunction`
// and `RecordException` — driven through the real pipeline: SdkTracer →
// BatchSpanProcessor → IExporter. What the unit tests cannot show, because
// `FakeTracer`'s handles carry a no-op deleter, is that the scopes actually
// *end* their spans and that the ended records reach an exporter as a
// correctly-parented tree under one trace id.
//
// Scope, deliberately: **exactly one** integration test, and no conformance
// test at all. `microtel-spec.md` §18.1 — "Sugar APIs are explicitly non-goals
// for compatibility testing — conformance tests target the OTel-like API and
// wire output, not convenience wrappers" — so nothing under
// tests/conformance/ gains a sugar case (ICP 0028, "Restated exclusions").

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/sugar.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"

#include "mocks/mock_transport.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mt = microtel::sugar;

namespace
{

constexpr std::chrono::milliseconds kFlushTimeout{2000};
constexpr int kCartSize = 3;

// Pre-bound keys, spelled once, exactly as ICP 0028 §2 intends them.
constexpr mt::AttrKey kHttpMethod{"http.method"};
constexpr mt::AttrKey kHttpRoute{"http.route"};
constexpr mt::AttrKey kCartItems{"cart.items"};

/// Capturing `IExporter`: keeps every record the batch processor hands over.
/// The mutex is for the processor's worker thread, which exports off the test
/// thread.
class CapturingExporter : public microtel::internal::IExporter
{
public:
    [[nodiscard]] microtel::internal::ExportResult Export(
        microtel::internal::BatchHandle&& batch) noexcept override
    {
        const std::scoped_lock lock{m_mu};
        for (const auto& record : batch.Spans())
        {
            m_records.push_back(record);
        }
        return microtel::internal::ExportResult::Success;
    }

    [[nodiscard]] microtel::Status ForceFlush(
        std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return microtel::Status::Completed;
    }

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        return microtel::Status::Completed;
    }

    /// The first exported record whose name contains @p needle, or nullopt.
    [[nodiscard]] std::optional<microtel::internal::SpanRecord> Find(std::string_view needle) const
    {
        const std::scoped_lock lock{m_mu};
        for (const auto& record : m_records)
        {
            if (record.name.find(needle) != std::string::npos)
            {
                return record;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t Count() const
    {
        const std::scoped_lock lock{m_mu};
        return m_records.size();
    }

private:
    mutable std::mutex m_mu;
    std::vector<microtel::internal::SpanRecord> m_records;
};

/// The deepest frame. `TraceFunction` rather than the macro because this one
/// needs the span itself, for `AttrKey::Set` and `RecordException`.
void QuoteTheCartInTheDeepestFrame(microtel::Tracer& tracer, const int items)
{
    const microtel::ScopedSpan scope = mt::TraceFunction(tracer);
    kCartItems.Set(*scope, std::int64_t{items});

    try
    {
        throw std::runtime_error{"pricing service unreachable"};
    }
    catch (const std::runtime_error& e)
    {
        mt::RecordException(*scope, e);
    }
}

/// The macro frame: fire-and-forget, which is the whole point of it.
int PriceTheCartInAHelperFrame(microtel::Tracer& tracer, const int items)
{
    MICROTEL_TRACE_FUNCTION(tracer);
    QuoteTheCartInTheDeepestFrame(tracer, items);
    return items * 2;
}

/// Owns a real provider whose span pipeline is a real `BatchSpanProcessor`
/// over `CapturingExporter`. `SdkProvider` declares `m_exporter` before
/// `m_processor`, so the borrowed exporter outlives the processor that
/// borrows it.
struct PipelineFixture
{
    CapturingExporter* exporter = nullptr;
    std::shared_ptr<microtel::sdk::SdkProvider> provider;

    PipelineFixture()
    {
        auto owned_exporter = std::make_unique<CapturingExporter>();
        exporter = owned_exporter.get();
        const auto resource = std::make_shared<const microtel::Resource>();

        auto processor = std::make_unique<microtel::sdk::BatchSpanProcessor>(
            exporter, resource, microtel::BatchOptions{});

        provider = std::make_shared<microtel::sdk::SdkProvider>(microtel::sdk::SdkProviderArgs{
            .diagnostics = std::make_unique<microtel::sdk::DiagnosticsCounters>(),
            .encoder = nullptr,
            .auth = nullptr,
            .transport = std::make_unique<microtel::testing::MockTransport>(),
            .codec = nullptr,
            .exporter = std::move(owned_exporter),
            .processor = std::move(processor),
            .resource = resource,
            .sampler = microtel::MakeAlwaysOnSampler(),
            .span_limits = {},
            .connect_opts = {},
        });
    }
};

}  // namespace

// A four-deep tree, each level opened by a different sugar helper and exported
// through the real batch processor. The assertions below are this packet's
// span-tree evidence.
TEST(SugarPipeline, ATracedCallTreeExportsCorrectlyParentedSpans)
{
    const PipelineFixture f;
    const std::shared_ptr<microtel::Tracer> tracer = f.provider->GetTracer("sugar.demo", "1.1.0");

    int priced = 0;
    {
        // Level 1 — mt::Span, with inline AttrKey-built attributes.
        const microtel::ScopedSpan request =
            mt::Span(*tracer,
                     "http.request",
                     {kHttpMethod(std::string{"POST"}), kHttpRoute(std::string{"/checkout"})},
                     microtel::SpanKind::Server);

        // Level 2 — mt::Traced, whose return value must survive its scope.
        // Levels 3 and 4 are opened inside the callable.
        priced = mt::Traced(*tracer,
                            "checkout",
                            [&tracer] { return PriceTheCartInAHelperFrame(*tracer, kCartSize); });
    }
    ASSERT_EQ(f.provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    EXPECT_EQ(priced, kCartSize * 2);
    ASSERT_EQ(f.exporter->Count(), 4U);

    const auto request = f.exporter->Find("http.request");
    const auto checkout = f.exporter->Find("checkout");
    const auto priced_frame = f.exporter->Find("PriceTheCartInAHelperFrame");
    const auto quote_frame = f.exporter->Find("QuoteTheCartInTheDeepestFrame");
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(checkout.has_value());
    ASSERT_TRUE(priced_frame.has_value());
    ASSERT_TRUE(quote_frame.has_value());

    // One trace, four generations:
    //   http.request → checkout → PriceTheCart… → QuoteTheCart…
    EXPECT_FALSE(request->parent_context.IsValid());
    EXPECT_EQ(checkout->context.trace_id.AsBytes(), request->context.trace_id.AsBytes());
    EXPECT_EQ(priced_frame->context.trace_id.AsBytes(), request->context.trace_id.AsBytes());
    EXPECT_EQ(quote_frame->context.trace_id.AsBytes(), request->context.trace_id.AsBytes());
    EXPECT_EQ(checkout->parent_context.span_id.AsBytes(), request->context.span_id.AsBytes());
    EXPECT_EQ(priced_frame->parent_context.span_id.AsBytes(), checkout->context.span_id.AsBytes());
    EXPECT_EQ(quote_frame->parent_context.span_id.AsBytes(),
              priced_frame->context.span_id.AsBytes());

    // mt::Span's kind and its inline AttrKey attributes reached the record.
    EXPECT_EQ(request->kind, microtel::SpanKind::Server);
    ASSERT_EQ(request->attributes.size(), 2U);
    EXPECT_EQ(request->attributes[0].key, "http.method");
    EXPECT_EQ(std::get<std::string>(request->attributes[0].value), "POST");
    EXPECT_EQ(request->attributes[1].key, "http.route");
    EXPECT_EQ(std::get<std::string>(request->attributes[1].value), "/checkout");

    // AttrKey::Set and RecordException landed on the deepest span.
    ASSERT_EQ(quote_frame->attributes.size(), 1U);
    EXPECT_EQ(quote_frame->attributes[0].key, "cart.items");
    EXPECT_EQ(std::get<std::int64_t>(quote_frame->attributes[0].value), kCartSize);
    EXPECT_EQ(quote_frame->status_code, microtel::StatusCode::Error);
    EXPECT_EQ(quote_frame->status_description, "pricing service unreachable");
    ASSERT_EQ(quote_frame->events.size(), 1U);
    EXPECT_EQ(quote_frame->events[0].name, "exception");
    ASSERT_EQ(quote_frame->events[0].attributes.size(), 2U);
    EXPECT_EQ(quote_frame->events[0].attributes[0].key, "exception.type");
    EXPECT_EQ(std::get<std::string>(quote_frame->events[0].attributes[1].value),
              "pricing service unreachable");
}
