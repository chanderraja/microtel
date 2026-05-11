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
    return tools::RunPreflight(static_cast<int>(argv.size()),
                               const_cast<char**>(argv.data()),
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
