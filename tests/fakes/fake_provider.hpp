// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"

#include "fakes/fake_tracer.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `microtel::Provider` that records tracer acquisitions and
///        flush/shutdown calls.
///
/// `GetTracer` returns the single shared `tracer` regardless of scope so the
/// test can inspect what the returned tracer was asked to do; the requested
/// `(name, version)` pairs are recorded in `tracer_requests`. Meters and
/// loggers are out of scope for the otelcpp trace shim and return null.
/// Single-threaded use only.
class FakeProvider : public microtel::Provider
{
public:
    struct ScopeRequest
    {
        std::string name;
        std::string version;
    };

    std::shared_ptr<FakeTracer> tracer = std::make_shared<FakeTracer>();
    std::vector<ScopeRequest> tracer_requests;
    std::vector<std::chrono::milliseconds> force_flush_calls;
    std::vector<std::chrono::milliseconds> shutdown_calls;
    microtel::Status flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    [[nodiscard]] std::shared_ptr<microtel::Tracer> GetTracer(std::string_view name,
                                                              std::string_view version) override
    {
        tracer_requests.push_back({.name = std::string{name}, .version = std::string{version}});
        return tracer;
    }

    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect() override
    {
        return {};
    }

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override
    {
        force_flush_calls.push_back(timeout);
        return flush_result;
    }

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override
    {
        shutdown_calls.push_back(timeout);
        return shutdown_result;
    }

    [[nodiscard]] microtel::HealthSnapshot GetExporterHealth() const noexcept override
    {
        return {};
    }

    [[nodiscard]] std::shared_ptr<microtel::Meter> GetMeter(
        std::string_view /*name*/,
        std::string_view /*version*/,
        std::string_view /*schema_url*/) override
    {
        return nullptr;
    }

    [[nodiscard]] std::shared_ptr<microtel::Logger> GetLogger(std::string_view /*name*/,
                                                              std::string_view /*version*/) override
    {
        return nullptr;
    }
};

}  // namespace microtel::testing
