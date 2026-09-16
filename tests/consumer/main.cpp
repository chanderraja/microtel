// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// microtel consumer smoke test — ICP 0020 Decision 6.
//
// What a consumer of an *installed* microtel looks like: find_package, link
// microtel::microtel, include <microtel/…>, done. It doubles as the canonical
// find_package example (docs/development.md §12), so it stays short enough to
// read in one screen.
//
// What it proves about the install tree, none of which any in-tree test can:
//
//   - the public headers are all installed and still include one another
//     correctly from their installed locations (including internal/ and the
//     vendored tl::expected under microtel/vendor/);
//   - the export set resolves the whole static closure at link time —
//     TraceId::ToHex lives in libmicrotel_api.a, and nothing but the export set
//     puts that archive on a consumer's link line;
//   - microtelConfig.cmake's find_dependency calls resolve the archives'
//     undefined references to zlib, OpenSSL and nghttp2;
//   - the result runs.
//
// It makes no network assumption. The endpoint is a closed port, the timeouts
// are a few hundred milliseconds, and a failed Connect() is the expected
// outcome — so it passes offline and cannot flake on a slow machine. The
// assertions are deliberately loose about exporter behaviour: this gate is
// about the package, and the unit, integration and conformance suites own
// behaviour.

#include <microtel/baggage.hpp>
#include <microtel/error.hpp>
#include <microtel/provider.hpp>
#include <microtel/resource_detectors.hpp>
#include <microtel/sdk_builder.hpp>
#include <microtel/span.hpp>
#include <microtel/status.hpp>
#include <microtel/trace.hpp>
#include <microtel/tracer.hpp>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace
{

// Nothing here is meant to reach a collector, and the gate should not spend
// seconds proving it.
constexpr std::chrono::milliseconds kFastTimeout{250};
constexpr std::chrono::milliseconds kFlushTimeout{500};
constexpr std::chrono::milliseconds kShutdownTimeout{500};

// Port 1 (tcpmux) needs root to bind, and nothing on a CI runner or a
// developer box does — so Connect() here is a connection refused, not a hang.
constexpr const char* kDeadEndpoint{"http://127.0.0.1:1"};

constexpr std::size_t kTraceIdHexChars{32};
constexpr std::size_t kSpanIdHexChars{16};

const char* StatusToString(const microtel::Status status) noexcept
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
    }
    return "Unknown";
}

bool Check(const bool ok, const char* what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    return ok;
}

}  // namespace

int main()
{
    const microtel::TimeoutOptions timeouts{.connect = kFastTimeout,
                                            .tls_handshake = kFastTimeout,
                                            .per_export = kFastTimeout,
                                            .retry_budget = kFastTimeout,
                                            .flush = kFlushTimeout,
                                            .shutdown = kShutdownTimeout};

    // The two built-in resource detectors, reached through the installed
    // <microtel/resource_detectors.hpp> and handed to the installed
    // WithResourceDetector. Both are v1.1 public surface, so both are export-set
    // material: an unresolved MakeProcessDetector here is exactly the kind of
    // missing-archive defect this gate exists to catch.
    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(kDeadEndpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-consumer-smoke")
                     .WithServiceVersion("0.1.0")
                     .WithResourceDetector(microtel::MakeProcessDetector())
                     .WithResourceDetector(microtel::MakeHostDetector())
                     .WithTimeouts(timeouts)
                     .Build();

    if (!built)
    {
        std::cerr << "FAIL SdkBuilder::Build(): " << built.error().message << '\n';
        return 1;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    bool ok = Check(provider != nullptr, "Build() returned a provider");

    // A closed port must fail, and fail cleanly rather than hanging or
    // aborting. A *successful* Connect() would mean something is listening on
    // port 1, which makes the rest of this run untrustworthy rather than
    // better — so it is a failure here, not a bonus.
    const auto connected = provider->Connect();
    ok = Check(!connected, "Connect() to a closed port failed cleanly") && ok;

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-consumer-smoke", "0.1.0");
    ok = Check(tracer != nullptr, "GetTracer() returned a tracer") && ok;

    const microtel::SpanHandle span = tracer->StartSpan(
        "consumer.smoke",
        {.kind = microtel::SpanKind::Internal, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("consumer.smoke", std::string{"installed-tree"});

    // ToHex is the reason microtel_api is in the export set. An unresolved
    // symbol here is precisely the defect Decision 6 exists to catch.
    const microtel::SpanContext ctx = span->GetContext();
    const std::string trace_hex = ctx.trace_id.ToHex();
    const std::string span_hex = ctx.span_id.ToHex();
    span->End();

    std::cout << "trace_id=" << trace_hex << " span_id=" << span_hex << '\n';
    ok = Check(trace_hex.size() == kTraceIdHexChars, "TraceId::ToHex is 32 hex chars") && ok;
    ok = Check(span_hex.size() == kSpanIdHexChars, "SpanId::ToHex is 16 hex chars") && ok;

    // <microtel/baggage.hpp> is new public surface in v1.1, and it installs
    // only because the package installs include/microtel as a directory
    // (ICP 0020 Decision 2). Parsing here proves both halves: the header is in
    // the install tree, and Baggage::FromHeader resolves out of
    // libmicrotel_api.a, which nothing but the export set puts on this link
    // line.
    const microtel::Baggage bag = microtel::Baggage::FromHeader("tenant=acme,region=eu%2Dwest");
    ok = Check(bag.Get("tenant") == std::string_view{"acme"}, "Baggage::FromHeader round-trips") &&
         ok;

    // Nothing can reach a closed port, so TimedOut is the expected outcome and
    // Failed is an honest one. Only AlreadyShutDown is wrong — we have not
    // shut down.
    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush = " << StatusToString(flush) << '\n';
    ok = Check(flush != microtel::Status::AlreadyShutDown, "ForceFlush returned a live status") &&
         ok;

    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "Shutdown = " << StatusToString(shutdown) << '\n';
    ok = Check(shutdown == microtel::Status::Completed || shutdown == microtel::Status::TimedOut,
               "Shutdown completed or timed out") &&
         ok;

    std::cout << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
