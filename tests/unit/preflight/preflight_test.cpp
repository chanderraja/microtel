// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for M6-D: preflight CLI argument parsing and config-error paths.
// Network-dependent paths (connect/export against a real endpoint) are
// integration tests; these tests drive only the argument-validation and
// config-validation layers.

#include "preflight/preflight.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Helper: build a char** argv from an initializer list and call RunPreflight.
// ---------------------------------------------------------------------------

int Invoke(std::initializer_list<const char*> args,
           std::ostringstream& out,
           std::ostringstream& err)
{
    std::vector<const char*> argv(args);
    return tools::RunPreflight(
        static_cast<int>(argv.size()),
        const_cast<char**>(argv.data()),  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        out,
        err);
}

}  // namespace

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

TEST(PreflightTest, NoArgs_ReturnsUsageError)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(Invoke({"microtel-preflight"}, out, err), tools::kExitUsage);
    EXPECT_FALSE(err.str().empty());
}

TEST(PreflightTest, UnknownMode_ReturnsUsageError)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(Invoke({"microtel-preflight", "--preflight=foo"}, out, err), tools::kExitUsage);
    EXPECT_FALSE(err.str().empty());
}

TEST(PreflightTest, MissingPrefix_ReturnsUsageError)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(Invoke({"microtel-preflight", "connect"}, out, err), tools::kExitUsage);
    EXPECT_FALSE(err.str().empty());
}

TEST(PreflightTest, EmptyMode_ReturnsUsageError)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(Invoke({"microtel-preflight", "--preflight="}, out, err), tools::kExitUsage);
    EXPECT_FALSE(err.str().empty());
}

// ---------------------------------------------------------------------------
// Config validation — no endpoint set → EndpointMalformed → kExitConfig
// ---------------------------------------------------------------------------

TEST(PreflightTest, ConnectMode_NoEndpoint_ReturnsConfigError)
{
    // No config file, no OTEL_EXPORTER_OTLP_ENDPOINT env var → invalid config.
    std::ostringstream out;
    std::ostringstream err;
    // Unset the env var in case it is set in the test environment.
    ::unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    EXPECT_EQ(Invoke({"microtel-preflight", "--preflight=connect"}, out, err), tools::kExitConfig);
    EXPECT_FALSE(err.str().empty());
}

TEST(PreflightTest, ExportMode_NoEndpoint_ReturnsConfigError)
{
    std::ostringstream out;
    std::ostringstream err;
    ::unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    EXPECT_EQ(Invoke({"microtel-preflight", "--preflight=export"}, out, err), tools::kExitConfig);
    EXPECT_FALSE(err.str().empty());
}

TEST(PreflightTest, ConnectMode_NonexistentConfigFile_ReturnsConfigError)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(
        Invoke({"microtel-preflight", "--preflight=connect", "/nonexistent/path/microtel.toml"},
               out,
               err),
        tools::kExitConfig);
    EXPECT_FALSE(err.str().empty());
}

// ---------------------------------------------------------------------------
// Config file path is accepted (file content may still fail validation)
// ---------------------------------------------------------------------------

TEST(PreflightTest, ConnectMode_MalformedEndpointViaEnv_ReturnsConfigError)
{
    std::ostringstream out;
    std::ostringstream err;
    ::setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "not-a-url", /*overwrite=*/1);
    const int code = Invoke({"microtel-preflight", "--preflight=connect"}, out, err);
    ::unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    EXPECT_EQ(code, tools::kExitConfig);
}

// ---------------------------------------------------------------------------
// Synthetic span identity — spec §6.4, issue #209
//
// These assert on `ResolveSpanIdentity` rather than on an exported span
// because `Provider` has no accessor for its resolved config and no seam
// through which a built provider's `Resource` can be read. `RunPreflight`
// uses this same struct for the provider's service name and the span's
// `microtel.protocol` attribute, so it is the values themselves under test.
// ---------------------------------------------------------------------------

namespace
{

// Clears every environment variable these tests set, so one case cannot leak
// configuration into the next. gtest gives no ordering guarantee across TUs.
void ClearIdentityEnv()
{
    ::unsetenv("OTEL_SERVICE_NAME");
    ::unsetenv("OTEL_EXPORTER_OTLP_PROTOCOL");
    ::unsetenv("OTEL_EXPORTER_OTLP_ENDPOINT");
}

}  // namespace

TEST(PreflightSpanIdentityTest, ServiceNameIsMicrotelPreflight)
{
    ClearIdentityEnv();
    EXPECT_EQ(tools::ResolveSpanIdentity("").service_name, "microtel-preflight");
}

TEST(PreflightSpanIdentityTest, ServiceNameIsNotTheUnknownServiceFallback)
{
    // Regression guard for #209: with nothing naming a service, config
    // resolution supplies "unknown_service" (#203). preflight's span must
    // still identify itself, or a collector rule written against the spec's
    // value matches nothing.
    ClearIdentityEnv();
    EXPECT_NE(tools::ResolveSpanIdentity("").service_name, "unknown_service");
}

TEST(PreflightSpanIdentityTest, ServiceNameOverridesOtelServiceNameEnv)
{
    // A preflight run against an operator's configuration must report
    // preflight's identity, not that configuration's. Code overrides are the
    // highest-precedence layer (docs/configuration.md §2).
    ClearIdentityEnv();
    ::setenv("OTEL_SERVICE_NAME", "operators-own-service", /*overwrite=*/1);
    const auto identity = tools::ResolveSpanIdentity("");
    ClearIdentityEnv();
    EXPECT_EQ(identity.service_name, "microtel-preflight");
}

TEST(PreflightSpanIdentityTest, ProtocolDefaultsToHttp)
{
    ClearIdentityEnv();
    EXPECT_EQ(tools::ResolveSpanIdentity("").protocol, "http");
}

TEST(PreflightSpanIdentityTest, ProtocolReflectsGrpcFromEnv)
{
    ClearIdentityEnv();
    ::setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "grpc", /*overwrite=*/1);
    const auto identity = tools::ResolveSpanIdentity("");
    ClearIdentityEnv();
    EXPECT_EQ(identity.protocol, "grpc");
}

TEST(PreflightSpanIdentityTest, ProtocolReflectsGrpcFromEndpointScheme)
{
    // `grpc://` selects OTLP/gRPC when no protocol is named explicitly; the
    // reported attribute has to follow the same resolution.
    ClearIdentityEnv();
    ::setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "grpc://localhost:4317", /*overwrite=*/1);
    const auto identity = tools::ResolveSpanIdentity("");
    ClearIdentityEnv();
    EXPECT_EQ(identity.protocol, "grpc");
}

TEST(PreflightSpanIdentityTest, ProtocolReflectsHttpProtobufFromEnv)
{
    ClearIdentityEnv();
    ::setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "http/protobuf", /*overwrite=*/1);
    const auto identity = tools::ResolveSpanIdentity("");
    ClearIdentityEnv();
    EXPECT_EQ(identity.protocol, "http");
}
