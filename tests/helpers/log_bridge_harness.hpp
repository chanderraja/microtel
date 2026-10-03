// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/sampler.hpp"
#include "microtel/log_record.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_log_exporter.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::testing
{

/// @brief A real `SdkProvider` whose log pipeline ends in a `FakeLogExporter`.
///
/// Shared by the log-bridge tests (glog, log4cxx): they need the SDK's own
/// behaviour — trace correlation from the current context, post-shutdown drop
/// accounting — rather than a `FakeLogger` that has neither.
struct LogBridgeHarness
{
    LogBridgeHarness()
    {
        auto exporter = std::make_unique<FakeLogExporter>();
        log_exporter = exporter.get();
        // Assigned field by field: SdkProviderArgs has many defaulted members
        // this harness has no opinion on.
        sdk::SdkProviderArgs args;
        auto counters = std::make_unique<sdk::DiagnosticsCounters>();
        diagnostics = counters.get();
        args.diagnostics = std::move(counters);
        args.transport = std::make_unique<MockTransport>();
        args.exporter = std::make_unique<MockExporter>();
        args.processor = std::make_unique<MockSpanProcessor>();
        args.resource = std::make_shared<Resource>();
        args.sampler = MakeAlwaysOnSampler();
        args.log_exporter = std::move(exporter);
        provider = std::make_unique<sdk::SdkProvider>(std::move(args));
    }

    /// @brief Every record the exporter has received, in export order.
    ///
    /// Only meaningful after `provider->ForceFlush()` or `Shutdown()`.
    [[nodiscard]] std::vector<LogRecord> Exported() const
    {
        std::vector<LogRecord> out;
        for (const auto& batch : log_exporter->exported)
        {
            for (const auto& rec : batch.Records())
            {
                out.push_back(rec);
            }
        }
        return out;
    }

    /// @brief The provider's `PostShutdown` drop count.
    ///
    /// Read from the sink rather than the provider, so it stays readable after
    /// `provider` is reset — for as long as a logger the provider handed out
    /// is alive, since the logger keeps the sink alive (issue #417).
    [[nodiscard]] std::uint64_t PostShutdownDrops() const
    {
        return diagnostics->Snapshot()
            .drop_counters[static_cast<std::size_t>(DropReason::PostShutdown)];
    }

    std::unique_ptr<sdk::SdkProvider> provider;
    const FakeLogExporter* log_exporter = nullptr;
    /// The provider's diagnostics sink. Borrowed from it, so after `provider`
    /// is reset it is valid only while a logger from it is alive.
    const sdk::DiagnosticsCounters* diagnostics = nullptr;
};

/// @brief The string attribute `key` on `rec`, or `"<absent>"`.
///
/// A sentinel rather than an optional so an assertion on it reads as one
/// comparison and a missing attribute shows up as a value mismatch.
[[nodiscard]] inline std::string StringAttribute(const LogRecord& rec, std::string_view key)
{
    for (const auto& kv : rec.attributes)
    {
        const auto* value = std::get_if<std::string>(&kv.value);
        if (kv.key == key && value != nullptr)
        {
            return *value;
        }
    }
    return "<absent>";
}

/// @brief The integer attribute `key` on `rec`, or -1.
[[nodiscard]] inline std::int64_t IntAttribute(const LogRecord& rec, std::string_view key)
{
    for (const auto& kv : rec.attributes)
    {
        const auto* value = std::get_if<std::int64_t>(&kv.value);
        if (kv.key == key && value != nullptr)
        {
            return *value;
        }
    }
    return -1;
}

/// @brief A valid, sampled span context whose ids are all `seed`.
[[nodiscard]] inline SpanContext MakeSampledSpanContext(std::uint8_t seed)
{
    TraceId::Bytes trace_bytes{};
    trace_bytes.fill(seed);
    SpanId::Bytes span_bytes{};
    span_bytes.fill(seed);
    return SpanContext{
        .trace_id = TraceId{trace_bytes},
        .span_id = SpanId{span_bytes},
        .trace_flags = TraceFlags{TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };
}

}  // namespace microtel::testing
