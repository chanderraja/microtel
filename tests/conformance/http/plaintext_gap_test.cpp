// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Pins the plaintext OTLP/HTTP gap deliberately.
//
// microtel's transport is HTTP/2-only (nghttp2), so a plaintext `http://`
// endpoint means h2c with prior knowledge. The OpenTelemetry Collector's
// plaintext OTLP/HTTP receiver serves HTTP/1.1 only — it does not wrap its
// handler in `h2c` — so it answers the HTTP/2 connection preface with an
// HTTP/1.1 error and the SETTINGS exchange never completes. That is issue #166
// and docs/interop-matrix.md §4.
//
// This is an unusual test: it asserts a *limitation*. It exists because the
// limitation is load-bearing. It is the reason every other OTLP/HTTP
// conformance test in this directory points at a TLS endpoint, and the reason
// the collector config gives the bearer-auth HTTP receiver a server
// certificate it would otherwise not need. A reader who does not know that
// would reasonably assume the TLS endpoints were chosen to test TLS.
//
// **When this test fails, do not delete it — invert it.** A failure means
// exactly one of two good things happened: microtel gained an HTTP/1.1
// fallback (an open ICP question — the OTLP specification permits HTTP/1.1),
// or the pinned collector gained h2c on its plaintext receiver. Either way the
// right response is to turn this into a positive delivery test against
// MICROTEL_CONFORMANCE_HTTP_ENDPOINT, close issue #166, and update
// docs/interop-matrix.md §4 and the "why TLS" comments in the sibling tests.

#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include "conformance/support/conformance_env.hpp"
#include "conformance/support/provider_builder.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace
{

constexpr const char* kPlaintextEndpointEnv = "MICROTEL_CONFORMANCE_HTTP_ENDPOINT";

constexpr const char* kSkipReason =
    "no conformance collector configured — run ci/scripts/conformance.sh";

constexpr const char* kServiceName = "microtel-conformance";

TEST(HttpPlaintextGapConformance, PlaintextHttpUnreachable)
{
    std::string endpoint;
    if (!microtel::testing::ConformanceEnabled(kPlaintextEndpointEnv, endpoint))
    {
        GTEST_SKIP() << kSkipReason;
    }

    constexpr auto kShort = std::chrono::milliseconds(2000);

    microtel::SdkBuilder builder;
    auto result =
        microtel::testing::ConfigureConformanceBuilder(builder, endpoint, microtel::Protocol::Http)
            .WithServiceName(kServiceName)
            // No WithTls: this is the quick-start configuration —
            // `http://collector:4318` and nothing else — which is
            // precisely what issue #166 says cannot work.
            .WithTimeouts(microtel::TimeoutOptions{
                .connect = kShort,
                .tls_handshake = kShort,
                .per_export = kShort,
                // No retries: nothing here is going to start working
                // on a second attempt, and a budget spent on backoff
                // is wall clock the gate pays on every run.
                .retry_budget = std::chrono::milliseconds(1),
                .flush = std::chrono::seconds(10),
                .shutdown = std::chrono::seconds(5),
            })
            .Build();
    // Build() validates configuration, not reachability — a plaintext endpoint
    // is perfectly well-formed, so the gap cannot show up here.
    ASSERT_TRUE(result.has_value()) << result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*result);

    // The receiver's HTTP/1.1 response to our HTTP/2 preface is not a frame
    // nghttp2 can parse, and the transport sniffs exactly that case so the
    // failure names itself instead of arriving as a generic nghttp2 error.
    // Both halves are asserted: that the endpoint is unreachable, and that the
    // one thing an operator gets — the error message — says why and what to do
    // instead.
    const auto connected = provider->Connect();
    ASSERT_FALSE(connected.has_value())
        << "plaintext OTLP/HTTP reached the collector — issue #166 is fixed, so invert this test "
           "into a positive delivery test rather than deleting it";
    EXPECT_NE(connected.error().message.find("HTTP/1.1-only"), std::string::npos)
        << "the plaintext gap must surface as its own diagnostic, not a generic transport "
           "failure; message was: "
        << connected.error().message;

    EXPECT_NE(provider->GetExporterHealth().connection_state, microtel::ConnectionState::Connected);
}

}  // namespace
