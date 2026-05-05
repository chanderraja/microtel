// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Mocks smoke test.
//
// Compile + link verification for the dumb mocks under tests/mocks/. Each
// test instantiates one mock, exercises one method, and asserts the
// recording counter ticked. Real behavioural tests using these mocks
// land in M3 against the actual production code.

#include <gtest/gtest.h>

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_otlp_encoder.hpp"
#include "mocks/mock_sampler.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "mocks/mock_wire_codec.hpp"

namespace mt = microtel;

namespace {

TEST(MockTransport, RecordsCalls)
{
    mt::testing::MockTransport mock;
    mt::internal::ConnectOptions opts {};
    auto connect_rc = mock.Connect(opts);
    EXPECT_TRUE(connect_rc.has_value());
    EXPECT_EQ(mock.connect_call_count, 1);
    EXPECT_EQ(mock.GetState(), mt::ConnectionState::Connected);

    auto rc = mock.Close(std::chrono::milliseconds{0});
    EXPECT_EQ(rc, mt::Status::Completed);
    EXPECT_EQ(mock.close_call_count, 1);
}

TEST(MockOtlpEncoder, ReturnsConfiguredBytes)
{
    mt::testing::MockOtlpEncoder mock;
    mock.bytes_to_return = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

    mt::internal::BatchHandle batch {};
    auto payload = mock.Encode(batch);

    EXPECT_EQ(mock.encode_call_count, 1);
    EXPECT_EQ(payload.Size(), std::size_t{3});
    EXPECT_EQ(payload.Bytes()[0], std::byte{0x01});
    EXPECT_EQ(payload.Bytes()[2], std::byte{0x03});
}

TEST(MockWireCodec, ReturnsConfiguredResult)
{
    mt::testing::MockWireCodec mock;
    mock.result_to_return.success                  = true;
    mock.result_to_return.partial_success_rejected = 7;

    mt::internal::EncodedPayload empty {};
    auto result = mock.Send(std::move(empty), std::chrono::seconds{1});

    EXPECT_EQ(mock.send_call_count, 1);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.partial_success_rejected, std::uint32_t{7});
}

TEST(MockExporter, RecordsLifecycleCalls)
{
    mt::testing::MockExporter mock;
    mt::internal::BatchHandle batch {};
    auto rc = mock.Export(std::move(batch));
    EXPECT_EQ(rc, mt::internal::ExportResult::Success);
    EXPECT_EQ(mock.export_call_count, 1);

    auto flush_rc = mock.ForceFlush(std::chrono::seconds{1});
    EXPECT_EQ(flush_rc, mt::Status::Completed);
    EXPECT_EQ(mock.force_flush_call_count, 1);
}

TEST(MockSampler, ReturnsConfiguredDecision)
{
    mt::testing::MockSampler mock;
    mt::internal::SamplingContext ctx {};
    auto result = mock.ShouldSample(ctx);
    EXPECT_EQ(result.decision, mt::internal::SamplingDecision::RecordAndSample);
    EXPECT_EQ(mock.should_sample_call_count, 1);
    EXPECT_EQ(mock.Description(), std::string_view{"MockSampler"});
}

TEST(MockSpanProcessor, RecordsLifecycleCalls)
{
    mt::testing::MockSpanProcessor mock;
    auto rc = mock.Shutdown(std::chrono::seconds{1});
    EXPECT_EQ(rc, mt::Status::Completed);
    EXPECT_EQ(mock.shutdown_call_count, 1);
}

}  // namespace
