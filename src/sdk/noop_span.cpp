// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// NoopSpan singleton and out-of-line SpanDeleter::operator() definition.
// Both must live in the same TU because SpanDeleter needs the full Span
// definition to instantiate unique_ptr<Span, SpanDeleter>.

#include "sdk/noop_span.hpp"

#include "microtel/attribute.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <string_view>

namespace
{

/// @brief Zero-cost no-op span; never heap-allocated.
///
/// A single process-lifetime static instance is shared by all unsampled
/// SpanHandle values. The SpanDeleter for these handles holds `nullptr`,
/// so unique_ptr never calls delete on this instance.
class NoopSpan final : public microtel::Span
{
public:
    NoopSpan() noexcept = default;
    ~NoopSpan() noexcept override = default;

    NoopSpan(const NoopSpan&) = delete;
    NoopSpan& operator=(const NoopSpan&) = delete;
    NoopSpan(NoopSpan&&) noexcept = delete;
    NoopSpan& operator=(NoopSpan&&) noexcept = delete;

    [[nodiscard]] microtel::SpanContext GetContext() const noexcept override
    {
        return {};
    }

    [[nodiscard]] bool IsSampled() const noexcept override
    {
        return false;
    }

    void SetAttribute(std::string_view /*key*/,
                      microtel::AttributeValue /*value*/) noexcept override
    {
    }

    void AddEvent(std::string_view /*name*/,
                  microtel::AttributeSpan /*attributes*/,
                  std::chrono::system_clock::time_point /*timestamp*/) noexcept override
    {
    }

    void AddLink(const microtel::SpanContext& /*linked_context*/,
                 microtel::AttributeSpan /*attributes*/) noexcept override
    {
    }

    void SetStatus(microtel::StatusCode /*code*/,
                   std::string_view /*description*/) noexcept override
    {
    }

    void UpdateName(std::string_view /*name*/) noexcept override {}

    void End(std::chrono::system_clock::time_point /*end_time*/) noexcept override {}
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
NoopSpan g_noop_instance;

}  // namespace

namespace microtel
{

namespace internal
{

void SpanDeleter::operator()(Span* span) const noexcept
{
    if (deleter != nullptr)
    {
        deleter(span);
    }
}

}  // namespace internal

namespace sdk
{

SpanHandle MakeNoopHandle() noexcept
{
    return SpanHandle{&g_noop_instance, internal::SpanDeleter{nullptr}};
}

}  // namespace sdk

}  // namespace microtel
