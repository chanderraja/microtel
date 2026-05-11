// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "preflight/preflight.hpp"

#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace tools
{

namespace
{

constexpr std::string_view kVersion = "1.0.0";
constexpr std::string_view kPreflightFlag = "--preflight=";

enum class Mode
{
    Connect,
    Export,
};

struct PreflightArgs
{
    Mode mode;
    std::optional<std::filesystem::path> config_path;
};

[[nodiscard]] std::optional<PreflightArgs>
ParseArgs(int argc, char** argv, std::ostream& err)
{
    if (argc < 2)
    {
        err << "Usage: microtel-preflight --preflight={connect|export} [config.toml]\n";
        return std::nullopt;
    }

    const std::string_view arg1{argv[1]};
    if (arg1.substr(0, kPreflightFlag.size()) != kPreflightFlag)
    {
        err << "error: expected --preflight={connect|export}, got: " << arg1 << "\n";
        return std::nullopt;
    }

    const std::string_view mode_str = arg1.substr(kPreflightFlag.size());
    Mode mode;
    if (mode_str == "connect")
    {
        mode = Mode::Connect;
    }
    else if (mode_str == "export")
    {
        mode = Mode::Export;
    }
    else
    {
        err << "error: unknown preflight mode '" << mode_str
            << "' (expected 'connect' or 'export')\n";
        return std::nullopt;
    }

    std::optional<std::filesystem::path> config_path;
    if (argc >= 3)
    {
        config_path = std::filesystem::path{argv[2]};
    }

    return PreflightArgs{.mode = mode, .config_path = std::move(config_path)};
}

}  // namespace

int RunPreflight(int argc, char** argv)
{
    return RunPreflight(argc, argv, std::cout, std::cerr);
}

int RunPreflight(int argc, char** argv, std::ostream& out, std::ostream& err)
{
    const auto args = ParseArgs(argc, argv, err);
    if (!args)
    {
        return kExitUsage;
    }

    // Build Provider from file → env → defaults.
    microtel::SdkBuilder builder;
    if (args->config_path)
    {
        builder.FromFile(*args->config_path);
    }

    auto provider_result = builder.Build();
    if (!provider_result)
    {
        err << "error: configuration failed: " << provider_result.error().message << "\n";
        return kExitConfig;
    }

    auto& provider = *provider_result;

    // Establish connection.
    auto connect_result = provider->Connect();
    if (!connect_result)
    {
        err << "error: connect failed: " << connect_result.error().message << "\n";
        (void)provider->Shutdown(std::chrono::seconds(5));
        return kExitRuntime;
    }

    if (args->mode == Mode::Connect)
    {
        out << "connect OK\n";
        (void)provider->Shutdown(std::chrono::seconds(5));
        return kExitOk;
    }

    // Export mode: send one synthetic span tagged so collectors can drop it.
    auto tracer = provider->GetTracer("microtel-preflight", std::string{kVersion});
    {
        auto span = tracer->StartSpan("microtel.preflight");
        span->SetAttribute("microtel.preflight", true);
        span->SetAttribute("microtel.version", std::string{kVersion});
        // Protocol is not yet queryable from Provider; default to "http".
        // Revisit when Provider exposes its runtime config (v1.1).
        span->SetAttribute("microtel.protocol", std::string{"http"});
        span->End();
    }

    const auto status = provider->ForceFlush(std::chrono::seconds(10));
    (void)provider->Shutdown(std::chrono::seconds(5));

    if (status != microtel::Status::Completed)
    {
        err << "error: export timed out or failed\n";
        return kExitRuntime;
    }

    out << "export OK\n";
    return kExitOk;
}

}  // namespace tools
