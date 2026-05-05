// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <memory>

namespace microtel
{
class Span;
class Context;
}  // namespace microtel

namespace microtel::internal
{

/// @brief Synchronous, non-batching span processor (spec §8: "for tests/debug").
///
/// Each `OnEnd` builds a single-span `BatchHandle` (Resource + Scope from
/// construction) and hands it directly to the configured exporter. No
/// queue, no worker thread. `ForceFlush` is a no-op since nothing is
/// buffered; `Shutdown` delegates to the exporter.
///
/// `OnStart` is a no-op — v1 has no in-process span enrichment hooks
/// (those land in v1.4 per the roadmap).
///
/// **Lifetime.** The exporter pointer is non-owning; the caller (typically
/// a `Provider`) keeps the exporter alive for the processor's lifetime.
/// Resource ownership is shared (every batch produced by this processor
/// references the same Resource).
///
/// @threadsafety Thread-safe — the exporter contract permits concurrent
///               `Export` calls, and Resource is immutable post-construction.
class SimpleSpanProcessor final : public ISpanProcessor
{
public:
    SimpleSpanProcessor(IExporter* exporter,
                        std::shared_ptr<const microtel::Resource> resource,
                        InstrumentationScope scope) noexcept;

    ~SimpleSpanProcessor() noexcept override = default;

    SimpleSpanProcessor(const SimpleSpanProcessor&) = delete;
    SimpleSpanProcessor& operator=(const SimpleSpanProcessor&) = delete;
    SimpleSpanProcessor(SimpleSpanProcessor&&) = delete;
    SimpleSpanProcessor& operator=(SimpleSpanProcessor&&) = delete;

    void OnStart(microtel::Span& span, const microtel::Context& parent) noexcept override;
    void OnEnd(SpanRecord&& record) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    IExporter* m_exporter;
    std::shared_ptr<const microtel::Resource> m_resource;
    InstrumentationScope m_scope;
};

}  // namespace microtel::internal
