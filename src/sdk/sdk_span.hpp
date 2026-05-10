// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace microtel::sdk
{

/// @brief Sampled span that records attributes/events/links and calls
/// `ISpanProcessor::OnEnd` on `End()`.
///
/// Heap-allocated by `SdkTracer::StartSpan` on the sampled path.
/// The `SpanHandle` owning this span uses a deleter that calls `delete`.
///
/// All hot-path methods are `noexcept`. Limits from `SpanLimitOptions`
/// are enforced: excess attributes/events/links are silently dropped.
///
/// @threadsafety Not thread-safe — callers must serialise access per the
///               `Span` interface contract.
class SdkSpan final : public microtel::Span
{
public:
    SdkSpan(SpanContext context,
            SpanContext parent_context,
            std::string_view name,
            SpanKind kind,
            std::chrono::system_clock::time_point start_time,
            internal::ISpanProcessor* processor,
            std::shared_ptr<const Resource> resource,
            internal::InstrumentationScope scope,
            SpanLimitOptions limits) noexcept;

    ~SdkSpan() noexcept override;

    SdkSpan(const SdkSpan&) = delete;
    SdkSpan& operator=(const SdkSpan&) = delete;
    SdkSpan(SdkSpan&&) = delete;
    SdkSpan& operator=(SdkSpan&&) = delete;

    [[nodiscard]] SpanContext GetContext() const noexcept override;
    [[nodiscard]] bool IsSampled() const noexcept override;

    void SetAttribute(std::string_view key, AttributeValue value) noexcept override;
    void AddEvent(std::string_view name,
                  AttributeSpan attributes = {},
                  std::chrono::system_clock::time_point timestamp = {}) noexcept override;
    void AddLink(const SpanContext& linked_context,
                 AttributeSpan attributes = {}) noexcept override;
    void SetStatus(StatusCode code, std::string_view description = {}) noexcept override;
    void UpdateName(std::string_view name) noexcept override;
    void End(std::chrono::system_clock::time_point end_time = {}) noexcept override;

private:
    internal::ISpanProcessor* m_processor;
    std::shared_ptr<const Resource> m_resource;
    internal::InstrumentationScope m_scope;
    SpanLimitOptions m_limits;
    internal::SpanRecord m_record;
    std::atomic<bool> m_ended{false};
};

}  // namespace microtel::sdk
