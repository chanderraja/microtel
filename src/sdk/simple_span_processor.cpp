// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/simple_span_processor.hpp"

#include <new>
#include <utility>
#include <vector>

namespace microtel::internal
{

SimpleSpanProcessor::SimpleSpanProcessor(IExporter* exporter,
                                         std::shared_ptr<const microtel::Resource> resource,
                                         InstrumentationScope scope) noexcept
    : m_exporter(exporter), m_resource(std::move(resource)), m_scope(std::move(scope))
{
}

void SimpleSpanProcessor::OnStart(microtel::Span& /*span*/,
                                  const microtel::Context& /*parent*/) noexcept
{
    // No-op. v1 has no in-process span enrichment hooks.
}

void SimpleSpanProcessor::OnEnd(SpanRecord&& record) noexcept
{
    // ISpanProcessor::OnEnd is noexcept, but building the one-record batch
    // allocates. Without this guard a std::bad_alloc here terminates the host
    // process; dropping the span is the behaviour docs/error-model.md §2.2
    // requires. (No DropReason exists for allocation failure and adding one is
    // ICP-gated, so the drop is currently uncounted — see issue #134.)
    try
    {
        std::vector<SpanRecord> records;
        records.reserve(1);
        records.push_back(std::move(record));

        BatchHandle batch{std::move(records), m_resource, m_scope};
        (void)m_exporter->Export(std::move(batch));
    }
    // Dropping the span IS the documented behaviour (error-model.md §2.2);
    // there is nothing to handle, and rethrowing from a noexcept frame is the
    // terminate this guard exists to prevent.
    // NOLINTNEXTLINE(bugprone-empty-catch)
    catch (const std::bad_alloc&)
    {
    }
}

microtel::Status SimpleSpanProcessor::ForceFlush(std::chrono::milliseconds /*timeout*/) noexcept
{
    // Nothing buffered; complete immediately. Per the IExporter contract,
    // calling its ForceFlush would be a no-op too — but the simple
    // processor explicitly does NOT delegate, because that would conflate
    // "synchronous nothing-to-flush" with "exporter has its own buffers."
    return microtel::Status::Completed;
}

microtel::Status SimpleSpanProcessor::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    return m_exporter->Shutdown(timeout);
}

}  // namespace microtel::internal
