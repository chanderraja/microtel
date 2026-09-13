// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Bearer-token authentication over OTLP/gRPC, against a real collector.
//
// The collector's `bearertokenauth` extension guards the otlp/auth receiver
// pair, so these tests are the only place in the repo where an `Authorization`
// metadata entry is checked by something that did not also write the client. A
// unit test can assert we emitted the header; only a real authenticator can say
// the header was accepted — or, for the negative case, rejected with the status
// the retry classifier is supposed to act on.
//
// Two ways to supply the credential, both exercised, because they take
// different paths inside the SDK:
//
//   WithHeaders({{"authorization", ...}})  → StaticHeadersAuthProvider
//   WithAuthProvider(callback)             → CallbackAuthProvider, per-batch
//                                            with a TTL cache
//
// `AuthCallback` is documented in include/microtel/sdk_builder.hpp as
// "returning the current `Authorization` header value", and the gRPC codec
// pushes the returned string as the header value verbatim
// (src/wire/grpc/grpc_wire_codec.cpp:726) — no scheme is prepended. So the
// callback returns `"Bearer <token>"`, not the bare token.
//
// Why this receiver is PLAINTEXT, where its OTLP/HTTP twin is not
// ---------------------------------------------------------------
// The otlp/auth gRPC port (`:4347`) has no TLS, and that is the original design
// intent restored: a plaintext port isolates the `Authorization` header from
// the handshake entirely, so an auth failure cannot possibly be a trust
// failure. The OTLP/HTTP twin had to give that up — microtel cannot reach a
// plaintext collector HTTP receiver at all (issue #166), so `:4348` carries
// server TLS material and the HTTP tests buy the isolation back by pinning the
// correct CA in every test. gRPC needs neither workaround: gRPC is h2c by
// definition, so the negative test below differs from the positive ones in
// exactly one field — the token — with no TLS anywhere in the picture.

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kAuthEndpointEnv = "MICROTEL_CONFORMANCE_GRPC_AUTH_ENDPOINT";
constexpr const char* kTokenEnv = "MICROTEL_CONFORMANCE_AUTH_TOKEN";
constexpr const char* kOutputFileEnv = "MICROTEL_CONFORMANCE_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";
constexpr const char* kScopeName = "microtel.conformance.grpc.auth";
constexpr const char* kScopeVersion = "1.0";

/// The scheme the collector's `bearertokenauth` extension is configured with.
constexpr const char* kBearerPrefix = "Bearer ";
constexpr const char* kAuthorizationHeader = "authorization";

/// A token the extension will not recognise. Deliberately not a near-miss of
/// the real one: the point is a clean UNAUTHENTICATED, not a parser edge case.
constexpr const char* kWrongToken = "Bearer not-the-conformance-token";

/// What `HealthSnapshot::last_error_message` actually reads after the
/// collector's rejection: the codec's fixed string for every non-zero
/// `grpc-status`, not the status and not the collector's `grpc-message`
/// (issue #171). See the comment in WrongTokenRejected.
constexpr const char* kObservedGrpcErrorMessage = "grpc error";

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);

/// @brief Points @p builder at the collector's bearer-auth OTLP/gRPC receiver.
///
/// No `WithTls`: the receiver is plaintext on purpose, so nothing in this file
/// can confuse an auth outcome with a trust outcome. See the file comment.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint resolved from @ref kAuthEndpointEnv.
/// @return @p builder, so the caller can add credentials before `Build()`.
microtel::SdkBuilder& ConfigureAuthBuilder(microtel::SdkBuilder& builder,
                                           const std::string& endpoint)
{
    return microtel::testing::ConfigureConformanceBuilder(
               builder, endpoint, microtel::Protocol::Grpc)
        .WithServiceName(kServiceName);
}

/// @brief One `authorization: <value>` header, as `WithHeaders` wants it.
///
/// @param value the full header value, scheme included.
std::vector<microtel::KeyValue> AuthorizationHeader(std::string value)
{
    std::vector<microtel::KeyValue> headers;
    headers.push_back(microtel::KeyValue{.key = kAuthorizationHeader, .value = std::move(value)});
    return headers;
}

/// @brief Emits one uniquely-named span and asserts the collector wrote it out.
///
/// @param provider    borrowed; already built.
/// @param output_file the collector's traces.jsonl, from @ref kOutputFileEnv.
void ExpectSpanRoundTrip(microtel::Provider& provider, const std::string& output_file)
{
    const std::string marker = microtel::testing::UniqueMarker();
    {
        const auto tracer = provider.GetTracer(kScopeName, kScopeVersion);
        auto span = tracer->StartSpan(marker);
        span->End();
    }
    ASSERT_EQ(provider.ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider.GetExporterHealth();
    EXPECT_GE(health.batches_sent, 1U);
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;

    const auto line =
        microtel::testing::PollForLineContaining(output_file, marker, kCollectorPollTimeout);
    // value_or rather than assert-then-dereference: gtest's early return is
    // invisible to clang-tidy's dataflow, so a post-ASSERT access reads as
    // unchecked. A line that matched the marker is never empty.
    EXPECT_FALSE(line.value_or(std::string{}).empty())
        << "collector never wrote a line containing '" << marker << "' to " << output_file;
}

/// @brief Timeouts for the test that expects UNAUTHENTICATED.
///
/// `UNAUTHENTICATED (16)` is non-retryable per docs/error-model.md §7.2, so the
/// retry budget should never be spent — but if the classification ever
/// regressed to retryable, a generous budget would turn this test into a ctest
/// timeout instead of a clean failure. 1 ms makes the regression loud and fast.
microtel::TimeoutOptions FailFastTimeouts()
{
    constexpr auto kShort = std::chrono::milliseconds(2000);
    return microtel::TimeoutOptions{
        .connect = kShort,
        .tls_handshake = kShort,
        .per_export = kShort,
        .retry_budget = std::chrono::milliseconds(1),
        .flush = std::chrono::seconds(10),
        .shutdown = std::chrono::seconds(5),
    };
}

TEST(GrpcAuthConformance, StaticBearerHeader)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kAuthEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string token;
    if (!microtel::testing::ConformanceEnabled(kTokenEnv, token))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureAuthBuilder(builder, endpoint)
                      .WithHeaders(AuthorizationHeader(kBearerPrefix + token))
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;

    ExpectSpanRoundTrip(*provider, output_file);
}

TEST(GrpcAuthConformance, AuthCallback)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kAuthEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string token;
    if (!microtel::testing::ConformanceEnabled(kTokenEnv, token))
    {
        GTEST_SKIP() << kSkipReason;
    }

    // The callback returns the whole header value, scheme included — see the
    // file comment. Returning the bare token here would produce
    // `authorization: <token>`, which the collector answers with
    // UNAUTHENTICATED.
    const std::string header_value = kBearerPrefix + token;

    microtel::SdkBuilder builder;
    auto result =
        ConfigureAuthBuilder(builder, endpoint)
            .WithAuthProvider([header_value]() -> microtel::Expected<std::string, microtel::Error>
                              { return header_value; })
            .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;

    ExpectSpanRoundTrip(*provider, output_file);
}

TEST(GrpcAuthConformance, WrongTokenRejected)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kAuthEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureAuthBuilder(builder, endpoint)
                      .WithTimeouts(FailFastTimeouts())
                      .WithHeaders(AuthorizationHeader(kWrongToken))
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // Connect() succeeds: the collector does not authenticate at the HTTP/2
    // SETTINGS exchange, and there is no handshake here to fail for any other
    // reason. Authentication is a per-RPC decision, so the rejection can only
    // appear at export.
    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;

    const std::string marker = microtel::testing::UniqueMarker();
    {
        const auto tracer = provider->GetTracer(kScopeName, kScopeVersion);
        auto span = tracer->StartSpan(marker);
        span->End();
    }

    // Completed, not TimedOut: a rejected batch is a finished batch. ForceFlush
    // reports whether the queue drained, not whether the peer liked it.
    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_EQ(health.batches_sent, 0U);
    EXPECT_GE(health.batches_failed, 1U);

    // The operator-visible reason. Observed on the wire against the pinned
    // collector — `curl --http2-prior-knowledge` to this receiver with this
    // same bad credential returns:
    //
    //     :status 200
    //     grpc-status: 16
    //     grpc-message: provided authorization does not match expected scheme or token
    //
    // and what microtel makes of it:
    //
    //     batches_sent=0  batches_failed=1  last_error_message='grpc error'
    //
    // Not "UNAUTHENTICATED", not "16", and not the collector's own sentence —
    // `ClassifyGrpcCode` puts a single fixed string in `WireResult::error` for
    // every non-zero status (src/wire/grpc/grpc_wire_codec.cpp:606) and
    // `grpc-message` is never read anywhere in src/. That contradicts
    // docs/grpc-wire-protocol.md §4.4, which specifies the percent-decoded
    // `grpc-message` as `WireResult::error.message`, and it is filed as its own
    // defect (issue #171) rather than asserted-as-desired here, because this
    // suite asserts what the shipped code does. The HTTP sibling asserts
    // `"401"` appears in this same field; tighten this to match when #171 is
    // fixed.
    EXPECT_EQ(health.last_error_message, kObservedGrpcErrorMessage)
        << "the gRPC failure surface changed; if it now names the status, tighten this "
           "assertion the way tests/conformance/http/auth_test.cpp does";

    // The classification is right even though the message is thin: a
    // non-retryable status means the batch is not retried, so `batches_failed`
    // stays at 1 rather than climbing with the retry schedule.
    EXPECT_EQ(health.batches_failed, 1U);

    // Nothing got through. Not a poll: the claim is that no line ever appears,
    // and the successful tests above have already established the collector is
    // writing promptly.
    EXPECT_EQ(microtel::testing::CountOccurrences(output_file, marker), 0U);
}

// The drop accounting docs/error-model.md §7.2 specifies for
// `UNAUTHENTICATED (16)` (→ counter `non_retryable_failure`). Split out from
// WrongTokenRejected rather than folded into it, so a regression in the
// accounting is distinguishable from a regression in the classification.
//
// Ran disabled until issue #169 wired the delivery counters, exactly as on the
// OTLP/HTTP path.
TEST(GrpcAuthConformance, WrongTokenIncrementsNonRetryableDropCounter)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kAuthEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureAuthBuilder(builder, endpoint)
                      .WithTimeouts(FailFastTimeouts())
                      .WithHeaders(AuthorizationHeader(kWrongToken))
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    {
        const auto tracer = provider->GetTracer(kScopeName, kScopeVersion);
        auto span = tracer->StartSpan(microtel::testing::UniqueMarker());
        span->End();
    }
    ASSERT_EQ(provider->ForceFlush(kFlushTimeout), microtel::Status::Completed);

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    const auto index = static_cast<std::size_t>(microtel::DropReason::NonRetryableFailure);
    EXPECT_GE(health.drop_counters.at(index), 1U)
        << "UNAUTHENTICATED must be attributable through drop_counters, not only through "
           "batches_failed";
}

}  // namespace
