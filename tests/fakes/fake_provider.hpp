// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"

#include "fakes/fake_logger.hpp"
#include "fakes/fake_meter.hpp"
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
/// `GetTracer` / `GetMeter` / `GetLogger` return the single shared
/// `tracer` / `meter` / `logger` regardless of scope so tests can inspect
/// what the returned instance was asked to do; requested scopes are recorded
/// in `tracer_requests` / `meter_requests` (+ `meter_schema_urls`) /
/// `logger_requests`. Single-threaded use only.
class FakeProvider : public microtel::Provider
{
public:
    struct ScopeRequest
    {
        std::string name;
        std::string version;
    };

    std::shared_ptr<FakeTracer> tracer = std::make_shared<FakeTracer>();
    std::shared_ptr<FakeMeter> meter = std::make_shared<FakeMeter>();
    std::shared_ptr<FakeLogger> logger = std::make_shared<FakeLogger>();
    std::vector<ScopeRequest> tracer_requests;
    std::vector<ScopeRequest> meter_requests;
    std::vector<std::string> meter_schema_urls;
    std::vector<ScopeRequest> logger_requests;
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

    [[nodiscard]] std::shared_ptr<microtel::Meter> GetMeter(std::string_view name,
                                                            std::string_view version,
                                                            std::string_view schema_url) override
    {
        meter_requests.push_back({.name = std::string{name}, .version = std::string{version}});
        meter_schema_urls.emplace_back(schema_url);
        return meter;
    }

    [[nodiscard]] std::shared_ptr<microtel::Logger> GetLogger(std::string_view name,
                                                              std::string_view version) override
    {
        logger_requests.push_back({.name = std::string{name}, .version = std::string{version}});
        return logger;
    }
};

}  // namespace microtel::testing
