// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// ExportTransportCodec / ExportTransportChannel (ICP 0036): the SendResult →
// WireResult mapping, exception containment, the deadline and its shutdown
// clamp, Cancel, the connection state, and the oversized-request Warn.

#include "wire/custom/export_transport_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/export_transport.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"

#include "fakes/fake_export_transport.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "mocks/mock_export_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtw = microtel::wire;
namespace mtt = microtel::testing;

using namespace std::chrono_literals;

constexpr auto kPerExport = 10s;

mti::EncodedPayload PayloadOf(std::size_t n)
{
    auto buf = std::make_unique<std::byte[]>(n == 0 ? 1 : n);
    for (std::size_t i = 0; i < n; ++i)
    {
        buf[i] = static_cast<std::byte>(i & 0xFFU);
    }
    return mti::EncodedPayload{std::move(buf), n};
}

/// A channel over a transport the test keeps a borrowed pointer to.
template <typename Transport>
struct Rig
{
    Rig()
        : transport_ptr(std::make_unique<Transport>()),
          transport(transport_ptr.get()),
          channel(std::make_unique<mtw::ExportTransportChannel>(std::move(transport_ptr), &clock))
    {
    }

    mtt::FakeSteadyClock clock;
    std::unique_ptr<Transport> transport_ptr;
    Transport* transport = nullptr;
    std::unique_ptr<mtw::ExportTransportChannel> channel;
};

class LogCapture
{
public:
    LogCapture()
    {
        mt::SetLogSink(
            [this](mt::LogLevel level, std::string_view message)
            {
                if (level == mt::LogLevel::Warn)
                {
                    m_warns.emplace_back(message);
                }
            });
    }
    ~LogCapture()
    {
        mt::ResetLogSink();
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    [[nodiscard]] const std::vector<std::string>& Warns() const
    {
        return m_warns;
    }

private:
    std::vector<std::string> m_warns;
};

}  // namespace

// Every optional read below follows an ASSERT_TRUE(...has_value()), which
// returns from the test; the check cannot see through the gtest macro.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// ---------------------------------------------------------------------------
// SendResult → WireResult
// ---------------------------------------------------------------------------

TEST(ExportTransportCodecTest, Success_MapsToSuccessWithPartialRejectedCount)
{
    const Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return =
        mt::SendResult{.outcome = mt::SendOutcome::Success, .rejected = 3};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(PayloadOf(8), kPerExport);

    EXPECT_TRUE(r.success);
    EXPECT_FALSE(r.retryable);
    EXPECT_EQ(r.partial_success_rejected, 3U);
    EXPECT_FALSE(r.error.has_value());
    EXPECT_EQ(rig.transport->send_call_count.load(), 1);
}

TEST(ExportTransportCodecTest, Retryable_MapsToRetryableWithRetryAfterAndNetworkError)
{
    const Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return = mt::SendResult{
        .outcome = mt::SendOutcome::Retryable, .retry_after = 250ms, .message = "link busy"};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(PayloadOf(8), kPerExport);

    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.retryable);
    ASSERT_TRUE(r.retry_after.has_value());
    EXPECT_EQ(r.retry_after.value(), 250ms);
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error.value().kind, mt::Error::Kind::Network);
    EXPECT_EQ(r.error.value().message, "link busy");
}

TEST(ExportTransportCodecTest, Retryable_WithoutRetryAfter_LeavesItUnset)
{
    const Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return = mt::SendResult{.outcome = mt::SendOutcome::Retryable};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(PayloadOf(8), kPerExport);

    EXPECT_TRUE(r.retryable);
    EXPECT_FALSE(r.retry_after.has_value());
    ASSERT_TRUE(r.error.has_value());
    EXPECT_FALSE(r.error.value().message.empty()) << "a failure with no message still names itself";
}

TEST(ExportTransportCodecTest, NonRetryable_MapsToNonRetryableFailure)
{
    const Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return =
        mt::SendResult{.outcome = mt::SendOutcome::NonRetryable, .message = "too large"};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(PayloadOf(8), kPerExport);

    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.retryable);
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error.value().kind, mt::Error::Kind::Network);
    EXPECT_EQ(r.error.value().message, "too large");
}

TEST(ExportTransportCodecTest, Request_CarriesSignalBytesAndDeadline)
{
    Rig<mtt::FakeExportTransport> rig;
    rig.clock.now = mti::TimePointSteady{} + 100s;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Metrics};

    (void)codec.Send(PayloadOf(5), kPerExport);

    const auto sent = rig.transport->Sent();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].signal, mt::ExportSignal::Metrics);
    const auto expected = PayloadOf(5);
    EXPECT_EQ(sent[0].bytes,
              std::vector<std::byte>(expected.Bytes().begin(), expected.Bytes().end()));
    EXPECT_EQ(sent[0].deadline, rig.clock.now + kPerExport);
}

TEST(ExportTransportCodecTest, EmptyPayload_IsRetryableAndNeverReachesTheTransport)
{
    const Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return = mt::SendResult{.outcome = mt::SendOutcome::Success};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(mti::EncodedPayload{}, kPerExport);

    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.retryable);
    EXPECT_EQ(rig.transport->send_call_count.load(), 0);
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

TEST(ExportTransportCodecTest, StdThrow_IsContainedAsNonRetryableInternalFailure)
{
    const Rig<mtt::FakeExportTransport> rig;
    rig.transport->throw_std = true;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(PayloadOf(4), kPerExport);

    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.retryable);
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error.value().kind, mt::Error::Kind::InternalFailure);
    EXPECT_NE(r.error.value().message.find("link exploded"), std::string::npos);
}

TEST(ExportTransportCodecTest, NonStdThrow_IsContainedAsNonRetryableInternalFailure)
{
    const Rig<mtt::FakeExportTransport> rig;
    rig.transport->throw_non_std = true;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    const auto r = codec.Send(PayloadOf(4), kPerExport);

    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.retryable);
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error.value().kind, mt::Error::Kind::InternalFailure);
    EXPECT_NE(r.error.value().message.find("non-std"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Deadline and shutdown
// ---------------------------------------------------------------------------

TEST(ExportTransportCodecTest, Deadline_IsClampedToTheShutdownDeadline)
{
    Rig<mtt::FakeExportTransport> rig;
    rig.clock.now = mti::TimePointSteady{} + 100s;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    rig.channel->BeginShutdown(2s);
    (void)codec.Send(PayloadOf(4), kPerExport);

    const auto sent = rig.transport->Sent();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].deadline, rig.clock.now + 2s);
}

TEST(ExportTransportCodecTest, Deadline_ShorterPerExportIsNotLengthenedByTheClamp)
{
    Rig<mtt::FakeExportTransport> rig;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    rig.channel->BeginShutdown(60s);
    (void)codec.Send(PayloadOf(4), 1s);

    ASSERT_EQ(rig.transport->Sent().size(), 1U);
    EXPECT_EQ(rig.transport->Sent()[0].deadline, rig.clock.now + 1s);
}

TEST(ExportTransportCodecTest, BeginShutdown_FirstCallFixesTheDeadline)
{
    Rig<mtt::FakeExportTransport> rig;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    rig.channel->BeginShutdown(2s);
    rig.channel->BeginShutdown(9s);
    (void)codec.Send(PayloadOf(4), kPerExport);

    ASSERT_EQ(rig.transport->Sent().size(), 1U);
    EXPECT_EQ(rig.transport->Sent()[0].deadline, rig.clock.now + 2s);
}

TEST(ExportTransportCodecTest, PastTheShutdownDeadline_TheTransportIsNotCalled)
{
    Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return = mt::SendResult{.outcome = mt::SendOutcome::Success};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    rig.channel->BeginShutdown(1s);
    rig.clock.Advance(1s);
    const auto r = codec.Send(PayloadOf(4), kPerExport);

    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.retryable);
    EXPECT_EQ(rig.transport->send_call_count.load(), 0);
}

TEST(ExportTransportCodecTest, CancelInFlight_WithNothingInFlight_DoesNotCallCancel)
{
    const Rig<mtt::MockExportTransport> rig;
    rig.transport->result_to_return = mt::SendResult{.outcome = mt::SendOutcome::Success};
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    rig.channel->CancelInFlight();
    const auto r = codec.Send(PayloadOf(4), kPerExport);

    EXPECT_EQ(rig.transport->cancel_call_count.load(), 0);
    EXPECT_FALSE(r.success) << "a Send after the cancel point is refused";
    EXPECT_FALSE(r.retryable);
    EXPECT_EQ(rig.transport->send_call_count.load(), 0);
}

TEST(ExportTransportCodecTest, CancelInFlight_WakesABlockedSendExactlyOnce)
{
    const Rig<mtt::FakeExportTransport> rig;
    rig.transport->block_until_cancel = true;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    mti::WireResult result;
    std::thread worker{[&] { result = codec.Send(PayloadOf(4), kPerExport); }};
    ASSERT_TRUE(rig.transport->WaitForSends(1, 5s));

    rig.channel->CancelInFlight();
    rig.channel->CancelInFlight();
    worker.join();

    EXPECT_EQ(rig.transport->CancelCalls(), 1) << "Cancel is called at most once";
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.retryable);
}

// ---------------------------------------------------------------------------
// Connection state
// ---------------------------------------------------------------------------

TEST(ExportTransportCodecTest, ConnectionState_FollowsSendOutcomes)
{
    const Rig<mtt::FakeExportTransport> rig;
    auto& script = rig.transport->scripted_results;
    script.push_back(mt::SendResult{.outcome = mt::SendOutcome::Retryable});
    script.push_back(mt::SendResult{.outcome = mt::SendOutcome::Success});
    script.push_back(mt::SendResult{.outcome = mt::SendOutcome::NonRetryable});
    script.push_back(mt::SendResult{.outcome = mt::SendOutcome::Success});
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces};

    EXPECT_EQ(rig.channel->State(), mt::ConnectionState::Disconnected);
    (void)codec.Send(PayloadOf(4), kPerExport);
    EXPECT_EQ(rig.channel->State(), mt::ConnectionState::Disconnected)
        << "a failure before any success is still configuration or first contact";
    (void)codec.Send(PayloadOf(4), kPerExport);
    EXPECT_EQ(rig.channel->State(), mt::ConnectionState::Connected);
    (void)codec.Send(PayloadOf(4), kPerExport);
    EXPECT_EQ(rig.channel->State(), mt::ConnectionState::Reconnecting);
    (void)codec.Send(PayloadOf(4), kPerExport);
    EXPECT_EQ(rig.channel->State(), mt::ConnectionState::Connected);
    rig.channel->MarkClosed();
    EXPECT_EQ(rig.channel->State(), mt::ConnectionState::Closed);
}

// ---------------------------------------------------------------------------
// max_request_bytes
// ---------------------------------------------------------------------------

TEST(ExportTransportCodecTest, OversizedRequest_IsSentWholeWithOneRateLimitedWarn)
{
    const LogCapture capture;
    Rig<mtt::FakeExportTransport> rig;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces, 16, &rig.clock};

    (void)codec.Send(PayloadOf(16), kPerExport);
    EXPECT_TRUE(capture.Warns().empty()) << "a request at the cap is not over it";
    (void)codec.Send(PayloadOf(17), kPerExport);
    (void)codec.Send(PayloadOf(40), kPerExport);

    const auto sent = rig.transport->Sent();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[1].bytes.size(), 17U) << "sent whole";
    EXPECT_EQ(sent[2].bytes.size(), 40U);
    ASSERT_EQ(capture.Warns().size(), 1U) << "rate-limited";
    EXPECT_NE(capture.Warns()[0].find("17"), std::string::npos) << "names the size";
    EXPECT_NE(capture.Warns()[0].find("16"), std::string::npos) << "names the cap";

    rig.clock.Advance(61s);
    (void)codec.Send(PayloadOf(40), kPerExport);
    EXPECT_EQ(capture.Warns().size(), 2U) << "the next interval warns again";
}

TEST(ExportTransportCodecTest, NoCap_NeverWarns)
{
    const LogCapture capture;
    Rig<mtt::FakeExportTransport> rig;
    mtw::ExportTransportCodec codec{rig.channel.get(), mt::ExportSignal::Traces, 0, &rig.clock};

    (void)codec.Send(PayloadOf(100000), kPerExport);

    EXPECT_TRUE(capture.Warns().empty());
}

// NOLINTEND(bugprone-unchecked-optional-access)
