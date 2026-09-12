// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Transport security over OTLP/gRPC, against a real collector.
//
// The TLS machinery is shared with the OTLP/HTTP path — same `SslCtx`, same
// verification logic from ICP 0022 — so the question here is not "does TLS
// work" but "does it still work under the one thing gRPC changes about the
// handshake": ALPN. gRPC requires `h2`, and where the HTTP path can fall back
// on the collector negotiating `h2` for an ordinary HTTP/2 request, the gRPC
// receiver is a gRPC server whose ALPN and client-auth policy are Go's
// `grpc-go`, not `net/http`. A hand-rolled peer cannot answer that, because
// both sides of it are written by the same people who wrote the client.
//
// The receivers the runner exposes make the posture a property of the endpoint
// rather than of a restart:
//
//   otlp/tls  (4327)  server cert only          → TlsCustomCa, SniOverride
//   otlp/mtls (4337)  server cert + client CA   → MutualTls, and the negative
//                                                 case where no client cert is
//                                                 presented
//
// Both negative tests below assert a *failure*, which is the only kind of TLS
// assertion that can catch verification silently not happening: a client that
// accepts anything passes every positive test in this file. `UntrustedCaFails`
// in particular is the regression guard for ICP 0022 on the gRPC path — before
// that change, `ca_bundle` was loaded into a trust store that `SSL_VERIFY_NONE`
// never consulted, so this exact test would have connected happily to a server
// the configured CA does not vouch for.
//
// The negative tests run with a near-zero retry budget and short connect /
// handshake timeouts: a failure path that sits out its backoff schedule turns
// a two-second test into a ctest timeout with no diagnostic.

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

namespace
{

constexpr const char* kTlsEndpointEnv = "MICROTEL_CONFORMANCE_GRPC_TLS_ENDPOINT";
constexpr const char* kMtlsEndpointEnv = "MICROTEL_CONFORMANCE_GRPC_MTLS_ENDPOINT";
constexpr const char* kCaEnv = "MICROTEL_CONFORMANCE_CA";
constexpr const char* kWrongCaEnv = "MICROTEL_CONFORMANCE_WRONG_CA";
constexpr const char* kClientCertEnv = "MICROTEL_CONFORMANCE_CLIENT_CERT";
constexpr const char* kClientKeyEnv = "MICROTEL_CONFORMANCE_CLIENT_KEY";
constexpr const char* kOutputFileEnv = "MICROTEL_CONFORMANCE_OUTPUT_FILE";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";
constexpr const char* kScopeName = "microtel.conformance.grpc.tls";
constexpr const char* kScopeVersion = "1.0";

constexpr auto kFlushTimeout = std::chrono::seconds(30);
constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);

// The TLS endpoint's host, and the address SniOverride dials instead. The
// run's server certificate carries `DNS:localhost` and `IP:127.0.0.1`, so the
// two are the same socket reached under two different names.
constexpr const char* kTlsHostName = "localhost";
constexpr const char* kLoopbackAddress = "127.0.0.1";

/// @brief Reads an environment variable, or the empty string if unset.
///
/// Only used for the variables a test has already established are present via
/// `ConformanceEnabled`, or for optional material where empty is meaningful.
std::string EnvOrEmpty(const char* name)
{
    return microtel::testing::GetEnv(name).value_or(std::string{});
}

/// @brief Points @p builder at @p endpoint over OTLP/gRPC with test timeouts.
///
/// @param builder  caller-owned; `SdkBuilder` cannot be returned by value.
/// @param endpoint OTLP endpoint URL.
/// @return @p builder, so the caller can add TLS material before `Build()`.
microtel::SdkBuilder& ConfigureGrpcBuilder(microtel::SdkBuilder& builder,
                                           const std::string& endpoint)
{
    return microtel::testing::ConfigureConformanceBuilder(
               builder, endpoint, microtel::Protocol::Grpc)
        .WithServiceName(kServiceName);
}

/// @brief Timeouts for the tests that expect the handshake to fail.
///
/// A near-zero `retry_budget` is the important one: it stops the exporter
/// sleeping through a backoff schedule for a connection that is never going to
/// come up. Copied from tests/integration/sdk/exporter_health_test.cpp.
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

/// @brief Emits one uniquely-named span and asserts the collector wrote it out.
///
/// Every positive test in this file ends the same way — the handshake is only
/// interesting if a span survives it — so the claim lives here once.
///
/// @param provider    borrowed; already built and connected.
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

/// @brief Rewrites @p endpoint's host from `localhost` to `127.0.0.1`.
///
/// The SNI test needs an endpoint whose host is *not* the name in the
/// certificate, so that `sni_override` is the only thing that can make
/// verification succeed. Deriving it from the runner's own variable keeps the
/// port in one place.
///
/// @param endpoint the TLS endpoint URL; copied.
/// @return the same URL with the host replaced, or unchanged if the host was
///         not the expected name.
std::string WithLoopbackHost(std::string endpoint)
{
    const std::size_t pos = endpoint.find(kTlsHostName);
    if (pos != std::string::npos)
    {
        endpoint.replace(pos, std::string(kTlsHostName).size(), kLoopbackAddress);
    }
    return endpoint;
}

TEST(GrpcTlsConformance, TlsCustomCa)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kTlsEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureGrpcBuilder(builder, endpoint)
                      .WithTls(microtel::TlsOptions{
                          // The run's throwaway CA. System trust would reject
                          // this server, which is the point: the configured
                          // bundle is what makes the handshake succeed.
                          .ca_bundle = EnvOrEmpty(kCaEnv),
                          .client_cert = {},
                          .client_key = {},
                          .sni_override = {},
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;
    EXPECT_EQ(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);

    ExpectSpanRoundTrip(*provider, output_file);
}

TEST(GrpcTlsConformance, MutualTls)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kMtlsEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }
    std::string output_file;
    if (!microtel::testing::ConformanceEnabled(kOutputFileEnv, output_file))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureGrpcBuilder(builder, endpoint)
                      .WithTls(microtel::TlsOptions{
                          .ca_bundle = EnvOrEmpty(kCaEnv),
                          .client_cert = EnvOrEmpty(kClientCertEnv),
                          .client_key = EnvOrEmpty(kClientKeyEnv),
                          .sni_override = {},
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // The collector's mTLS receiver carries `client_ca_file`, so reaching
    // Connected here means the collector verified our client certificate — not
    // merely that we offered one.
    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;
    EXPECT_EQ(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);

    ExpectSpanRoundTrip(*provider, output_file);
}

TEST(GrpcTlsConformance, MutualTlsWithoutClientCertFails)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kMtlsEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureGrpcBuilder(builder, endpoint)
                      .WithTimeouts(FailFastTimeouts())
                      .WithTls(microtel::TlsOptions{
                          // Correct server trust, deliberately no client
                          // material: the only thing missing is the cert the
                          // receiver's client CA is there to check.
                          .ca_bundle = EnvOrEmpty(kCaEnv),
                          .client_cert = {},
                          .client_key = {},
                          .sni_override = {},
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // Observed against the pinned collector: `Connect()` fails, reporting
    // "nghttp2 recv failed during SETTINGS exchange" rather than a handshake
    // error — the same surface the OTLP/HTTP mTLS test records. That is TLS 1.3
    // doing what it does: the client finishes the handshake without waiting for
    // the server to accept its (absent) certificate, so the server's alert
    // lands on the next read, which is the read of the server's SETTINGS frame.
    // Either way the connection never becomes usable, which is the claim under
    // test; the message is not asserted because it is an artefact of when the
    // alert arrives.
    const auto connected = provider->Connect();
    ASSERT_FALSE(connected.has_value())
        << "the mTLS receiver accepted a connection with no client certificate";

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_NE(health.connection_state, microtel::ConnectionState::Connected);
    EXPECT_EQ(health.batches_sent, 0U);
}

TEST(GrpcTlsConformance, SniOverride)
{
    std::string tls_endpoint;
    if (!microtel::testing::ConformanceEnabled(kTlsEndpointEnv, tls_endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    // Dial the IP literal, present `localhost` as the name. ICP 0022 verifies
    // the peer against `sni_override` when set, so this is the configuration an
    // operator uses when the endpoint is an address but the certificate names a
    // service. It is also the only variant worth testing here: OpenSSL before
    // 3.2 does not match IP literals against `iPAddress` SANs, so a no-override
    // connect to 127.0.0.1 fails the hostname check on CI regardless of the
    // SAN the runner puts in the certificate.
    const std::string endpoint = WithLoopbackHost(tls_endpoint);

    microtel::SdkBuilder builder;
    auto result = ConfigureGrpcBuilder(builder, endpoint)
                      .WithTls(microtel::TlsOptions{
                          .ca_bundle = EnvOrEmpty(kCaEnv),
                          .client_cert = {},
                          .client_key = {},
                          .sni_override = kTlsHostName,
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    const auto connected = provider->Connect();
    ASSERT_TRUE(connected.has_value()) << connected.error().message;
    EXPECT_EQ(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);
}

TEST(GrpcTlsConformance, UntrustedCaFails)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kTlsEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    microtel::SdkBuilder builder;
    auto result = ConfigureGrpcBuilder(builder, endpoint)
                      .WithTimeouts(FailFastTimeouts())
                      .WithTls(microtel::TlsOptions{
                          // A second, unrelated self-signed CA generated by the
                          // runner. It signed nothing this server presents.
                          .ca_bundle = EnvOrEmpty(kWrongCaEnv),
                          .client_cert = {},
                          .client_key = {},
                          .sni_override = {},
                      })
                      .Build();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // The ICP 0022 regression guard on the gRPC path. Observed failure against
    // the pinned collector: "TLS certificate verification failed: unable to get
    // local issuer certificate". Before ICP 0022 this connected, because the
    // trust store was loaded and then never consulted.
    const auto connected = provider->Connect();
    ASSERT_FALSE(connected.has_value()) << "connected to a server no configured CA vouches for — "
                                           "peer verification is not happening";

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    EXPECT_NE(health.connection_state, microtel::ConnectionState::Connected);
    EXPECT_EQ(health.batches_sent, 0U);
}

}  // namespace
