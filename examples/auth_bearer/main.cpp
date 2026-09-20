// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// auth_bearer — exporting to a collector that requires a bearer token, both
// ways microtel supplies one, and both ways it can go wrong.
//
// Four phases against the same receiver:
//
//   1. WithHeaders, wrong token      the collector rejects the batch. The
//                                    rejection reaches `GetExporterHealth()`
//                                    as batches_failed + NonRetryableFailure,
//                                    with the status in last_error_message.
//   2. WithAuthProvider, failing     the callback returns an error, so the
//      callback                      header cannot be built and **the batch is
//                                    dropped rather than sent unauthenticated**
//                                    (issue #250). last_error_message is
//                                    prefixed "authorization header
//                                    unavailable:".
//   3. WithHeaders, right token      a static credential. Zero-allocation
//                                    StaticHeadersAuthProvider; no user code
//                                    runs per batch.
//   4. WithAuthProvider, right token the callback shape, for a credential that
//                                    rotates. Called per batch, memoised for
//                                    `cache_ttl`.
//
// Usage:
//   auth_bearer [endpoint] [token]
//
// [endpoint] defaults to http://localhost:5317 — the **opt-in** auth overlay
//            collector, not the shared stack's 4317:
//              examples/auth_bearer/up-auth.sh
// [token]    defaults to the overlay's configured token. Override it to watch
//            phases 3 and 4 fail the way phase 1 does.
//
// The value handed to either surface is the complete header value, scheme
// included: the gRPC codec writes it verbatim and prepends nothing. So it is
// "Bearer <token>", not "<token>".

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kDefaultEndpoint{"http://localhost:5317"};
constexpr const char* kDefaultToken{"microtel-example-token"};
constexpr const char* kWrongToken{"not-the-token"};
constexpr const char* kBearerPrefix{"Bearer "};
constexpr const char* kAuthorizationHeader{"authorization"};

constexpr const char* kServiceName{"microtel-auth-example"};
constexpr const char* kScopeVersion{"1.0.0"};

constexpr std::chrono::seconds kFlushTimeout{10};
constexpr std::chrono::seconds kShutdownTimeout{5};

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

const char* ConnectionStateToString(microtel::ConnectionState state) noexcept
{
    switch (state)
    {
        case microtel::ConnectionState::Disconnected:
            return "Disconnected";
        case microtel::ConnectionState::Connecting:
            return "Connecting";
        case microtel::ConnectionState::Connected:
            return "Connected";
        case microtel::ConnectionState::Reconnecting:
            return "Reconnecting";
        case microtel::ConnectionState::Closed:
            return "Closed";
    }
    return "Unknown";
}

/// @brief One `authorization: <value>` header, as `WithHeaders` wants it.
///
/// @param value the complete header value, scheme included.
std::vector<microtel::KeyValue> AuthorizationHeader(std::string value)
{
    std::vector<microtel::KeyValue> headers;
    headers.push_back(microtel::KeyValue{.key = kAuthorizationHeader, .value = std::move(value)});
    return headers;
}

std::size_t DropCount(const microtel::HealthSnapshot& health, microtel::DropReason reason)
{
    return health.drop_counters[static_cast<std::size_t>(reason)];
}

/// @brief The health fields this example can move. `health_and_backpressure/`
///        prints the whole snapshot with every drop counter named.
void PrintHealth(const microtel::HealthSnapshot& health)
{
    std::cout << "  health: connection_state=" << ConnectionStateToString(health.connection_state)
              << " batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed << " NonRetryableFailure="
              << DropCount(health, microtel::DropReason::NonRetryableFailure)
              << " ConnectFailure=" << DropCount(health, microtel::DropReason::ConnectFailure)
              << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "  last_error: " << health.last_error_message << '\n';
    }
}

using Configure = std::function<void(microtel::SdkBuilder&)>;

struct Phase
{
    const char* label;
    const char* profile;
    bool expect_export;  ///< true when the batch is expected to reach the collector
    Configure configure;
};

/// @brief Build a provider, emit one span, flush, and report health.
/// @return true when the batch was accepted (batches_sent moved, none failed).
bool RunPhase(const Phase& phase, const std::string& endpoint)
{
    std::cout << "\n=== " << phase.label << " ===\n";

    microtel::SdkBuilder builder;
    builder.WithEndpoint(endpoint)
        .WithProtocol(microtel::Protocol::Grpc)
        .WithServiceName(kServiceName)
        .WithServiceVersion(kScopeVersion)
        .WithProfileName(phase.profile);
    phase.configure(builder);

    auto built = builder.Build();
    if (!built)
    {
        std::cerr << "  Build() failed: " << built.error().message << '\n';
        return false;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    // The connection itself is unauthenticated — the token travels on the
    // export request, not on the handshake — so this succeeds even in the
    // phases that go on to be rejected.
    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "  Connect() failed: " << connected.error().message << '\n';
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-auth-example", kScopeVersion);
    const auto span = tracer->StartSpan(
        "auth.request",
        {.kind = microtel::SpanKind::Client, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("phase", std::string{phase.label});
    span->SetStatus(microtel::StatusCode::Ok);
    const std::string trace_id = span->GetContext().trace_id.ToHex();
    span->End();

    std::cout << "  trace_id: " << trace_id << '\n'
              << "  ForceFlush: " << StatusToString(provider->ForceFlush(kFlushTimeout)) << '\n';

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    PrintHealth(health);
    std::cout << "  Shutdown: " << StatusToString(provider->Shutdown(kShutdownTimeout)) << '\n';

    return health.batches_sent > 0 && health.batches_failed == 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};
    const std::string token{(argc > 2) ? argv[2] : kDefaultToken};
    const std::string header_value = kBearerPrefix + token;

    std::cout << "endpoint: " << endpoint
              << "\nstart the auth overlay with: examples/auth_bearer/up-auth.sh\n";

    const std::vector<Phase> phases{
        {.label = "phase 1: WithHeaders, wrong token",
         .profile = "auth-wrong-static",
         .expect_export = false,
         .configure = [](microtel::SdkBuilder& b)
         { b.WithHeaders(AuthorizationHeader(kBearerPrefix + std::string{kWrongToken})); }},

        {.label = "phase 2: WithAuthProvider, callback fails",
         .profile = "auth-callback-error",
         .expect_export = false,
         .configure =
             [](microtel::SdkBuilder& b)
         {
             // A credential source that is down. Returning the error (rather
             // than throwing) keeps the Error::Kind all the way to
             // last_error_message; a throw would arrive as InternalFailure.
             // Either way the batch is dropped, never sent unauthenticated.
             b.WithAuthProvider(
                 []() -> microtel::Expected<std::string, microtel::Error>
                 {
                     return microtel::make_unexpected(
                         microtel::Error{.kind = microtel::Error::Kind::Network,
                                         .message = "token endpoint unreachable",
                                         .os_errno = 0});
                 },
                 std::chrono::milliseconds{0});
         }},

        {.label = "phase 3: WithHeaders, right token",
         .profile = "auth-static",
         .expect_export = true,
         .configure = [&header_value](microtel::SdkBuilder& b)
         { b.WithHeaders(AuthorizationHeader(header_value)); }},

        {.label = "phase 4: WithAuthProvider, right token",
         .profile = "auth-callback",
         .expect_export = true,
         .configure =
             [&header_value](microtel::SdkBuilder& b)
         {
             // The simple case from docs/auth-callback-recipes.md §1: the
             // callback is a cached read, never a network call — it runs on an
             // exporter worker, under the provider's mutex, with no timeout
             // bounding it. cache_ttl = 0 because this cache is already the
             // only cache.
             b.WithAuthProvider(
                 [&header_value]() -> microtel::Expected<std::string, microtel::Error>
                 { return header_value; },
                 std::chrono::milliseconds{0});
         }},
    };

    bool as_documented = true;
    for (const Phase& phase : phases)
    {
        const bool exported = RunPhase(phase, endpoint);
        if (exported != phase.expect_export)
        {
            std::cerr << "  UNEXPECTED: this phase " << (exported ? "exported" : "did not export")
                      << ", which is not what it is written to demonstrate\n";
            as_documented = false;
        }
    }

    std::cout << "\nGrafana: http://localhost:3000 — the two successful phases' traces are "
                 "forwarded\n         by the overlay collector into the shared stack's Tempo.\n";

    return as_documented ? 0 : 2;
}
