// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Fakes smoke test.
//
// Compile + link verification for the logic-bearing fakes under
// tests/fakes/. Each test instantiates one fake, exercises one or two
// methods, asserts the recording/scripting machinery does what it claims.
// Real behavioural tests using these fakes land in M3 against actual
// production code.

#include "fakes/fake_auth_provider.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_exporter.hpp"
#include "fakes/fake_reactor.hpp"
#include "fakes/fake_resource_detector.hpp"
#include "fakes/fake_span_processor.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_transport.hpp"

#include <gtest/gtest.h>

namespace mt = microtel;

namespace
{

TEST(FakeClock, AdvanceMovesNowForward)
{
    mt::testing::FakeClock clock;
    const auto wall_t0 = clock.Now();
    clock.Advance(std::chrono::seconds{5});
    const auto wall_t1 = clock.Now();
    EXPECT_EQ(wall_t1 - wall_t0, std::chrono::seconds{5});
}

TEST(FakeSteadyClock, AdvanceMovesNowForward)
{
    mt::testing::FakeSteadyClock clock;
    auto t0 = clock.Now();
    clock.Advance(std::chrono::milliseconds{42});
    EXPECT_EQ(clock.Now() - t0, std::chrono::milliseconds{42});
}

TEST(FakeDiagnosticsSink, RecordsAndSnapshots)
{
    mt::testing::FakeDiagnosticsSink sink;
    sink.RecordDrop(mt::DropReason::QueueFull, 3);
    sink.RecordBatchSent();
    sink.SetQueueDepth(17);
    sink.SetConnectionState(mt::ConnectionState::Connected);

    auto snap = sink.Snapshot();
    EXPECT_EQ(snap.drop_counters[static_cast<std::size_t>(mt::DropReason::QueueFull)],
              std::uint64_t{3});
    EXPECT_EQ(snap.batches_sent, std::uint64_t{1});
    EXPECT_EQ(snap.batches_failed, std::uint64_t{0});
    EXPECT_EQ(snap.queue_depth_now, std::uint64_t{17});
    EXPECT_EQ(snap.connection_state, mt::ConnectionState::Connected);
}

TEST(FakeAuthProvider, ScriptedFifo)
{
    mt::testing::FakeAuthProvider auth;
    auth.scripted_responses.emplace_back(std::optional<std::string>{"first"});
    auth.scripted_responses.emplace_back(std::optional<std::string>{"second"});

    auto r1 = auth.GetAuthorization(mt::internal::TimePointSteady{});
    auto r2 = auth.GetAuthorization(mt::internal::TimePointSteady{});
    auto r3 = auth.GetAuthorization(mt::internal::TimePointSteady{});  // falls back

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r1.value().value_or(""), "first");
    EXPECT_EQ(r2.value().value_or(""), "second");
    EXPECT_FALSE(r3.value().has_value());  // default_value is empty optional
    EXPECT_EQ(auth.call_count, 3);
}

TEST(FakeResourceDetector, ReturnsConfiguredResource)
{
    mt::testing::FakeResourceDetector detector;
    detector.resource_to_return = mt::Resource{std::vector<mt::KeyValue>{
        mt::KeyValue{.key = "service.name", .value = mt::AttributeValue{std::string{"x"}}}}};

    auto r = detector.Detect();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value().Attributes().size(), std::size_t{1});
    EXPECT_EQ(detector.detect_call_count, 1);
    EXPECT_EQ(detector.Name(), std::string_view{"FakeResourceDetector"});
}

TEST(FakeResourceDetector, FailureSurface)
{
    mt::testing::FakeResourceDetector detector;
    detector.failure = mt::ConfigError{.kind = mt::ConfigError::Kind::EnvParseFailure,
                                       .field = "OTEL_RESOURCE_ATTRIBUTES",
                                       .message = "malformed"};

    auto r = detector.Detect();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, mt::ConfigError::Kind::EnvParseFailure);
}

TEST(FakeSpanProcessor, RecordsEndedSpans)
{
    mt::testing::FakeSpanProcessor proc;
    mt::internal::SpanRecord r1{};
    r1.name = "first";
    proc.OnEnd(std::move(r1));

    EXPECT_EQ(proc.received_spans.size(), std::size_t{1});
    EXPECT_EQ(proc.received_spans[0].name, "first");
}

TEST(FakeExporter, RecordsBatches)
{
    mt::testing::FakeExporter exporter;
    mt::internal::BatchHandle batch;
    auto rc = exporter.Export(std::move(batch));
    EXPECT_EQ(rc, mt::internal::ExportResult::Success);
    EXPECT_EQ(exporter.received_batches.size(), std::size_t{1});
}

TEST(FakeReactor, DispatchesScriptedEvents)
{
    mt::testing::FakeReactor reactor;
    int observed_fd = 0;
    mt::internal::EventMask observed_mask{};

    auto cb = [&](int fd, mt::internal::EventMask mask)
    {
        observed_fd = fd;
        observed_mask = mask;
    };
    auto rc = reactor.Register(7, mt::internal::EventMask::Read, cb);
    EXPECT_TRUE(rc.has_value());

    reactor.scripted_events.push_back(
        {7, mt::internal::EventMask::Read | mt::internal::EventMask::Write});

    auto n = reactor.WaitAndDispatch(mt::internal::TimePointSteady{});
    EXPECT_EQ(n, std::size_t{1});
    EXPECT_EQ(observed_fd, 7);
    EXPECT_TRUE(mt::internal::HasEvent(observed_mask, mt::internal::EventMask::Write));

    reactor.Wake();
    EXPECT_EQ(reactor.wake_count, 1);
}

TEST(FakeTransport, ScriptedResponses)
{
    mt::testing::FakeTransport transport;
    transport.default_response.success = true;  // opt in to a successful default

    mt::internal::TransportResult r1{};
    r1.success = true;
    mt::internal::TransportResult r2{};
    r2.success = false;
    transport.scripted_responses.push_back(r1);
    transport.scripted_responses.push_back(r2);

    auto h1 = transport.Send(mt::internal::RequestSpec{});
    auto h2 = transport.Send(mt::internal::RequestSpec{});
    auto h3 = transport.Send(mt::internal::RequestSpec{});  // falls back to default

    EXPECT_TRUE(h1.Future().get().success);
    EXPECT_FALSE(h2.Future().get().success);
    EXPECT_TRUE(h3.Future().get().success);  // default_response.success was set above
    EXPECT_EQ(transport.sent_specs.size(), std::size_t{3});
}

}  // namespace
