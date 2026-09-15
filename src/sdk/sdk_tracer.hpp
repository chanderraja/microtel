// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <memory>
#include <string_view>

namespace microtel::sdk
{

/// @brief Production `Tracer` implementation.
///
/// Asks the sampler on every `StartSpan` call. If the decision is `Drop`,
/// returns the noop singleton with zero allocation. Otherwise heap-allocates
/// an `SdkSpan`, calls `OnStart`, and returns an owning `SpanHandle`.
///
/// Non-owning references to the sampler and processor are borrowed — the
/// `Provider` (or test fixture) keeps them alive for the tracer's lifetime.
///
/// @threadsafety Thread-safe — concurrent `StartSpan` calls from multiple
///               threads are safe. The sampler and processor must themselves
///               be thread-safe.
class SdkTracer final : public microtel::Tracer
{
public:
    /// @param diagnostics non-owning diagnostics sink handed to every span
    ///        this tracer starts, or `nullptr` to disable drop accounting.
    ///        Borrowed for the tracer's lifetime.
    SdkTracer(internal::ISampler* sampler,
              internal::ISpanProcessor* processor,
              std::shared_ptr<const Resource> resource,
              internal::InstrumentationScope scope,
              SpanLimitOptions limits,
              internal::IDiagnosticsSink* diagnostics = nullptr) noexcept;

    ~SdkTracer() noexcept override = default;

    SdkTracer(const SdkTracer&) = delete;
    SdkTracer& operator=(const SdkTracer&) = delete;
    SdkTracer(SdkTracer&&) noexcept = default;
    SdkTracer& operator=(SdkTracer&&) noexcept = default;

    [[nodiscard]] SpanHandle StartSpan(std::string_view name,
                                       const StartSpanOptions& opts = {}) noexcept override;

    [[nodiscard]] ScopedSpan StartAsCurrentSpan(std::string_view name,
                                                const StartSpanOptions& opts = {}) noexcept override;

private:
    /// @brief The whole of span creation, with the computed `SpanContext`
    ///        published to @p started.
    ///
    /// The two public entry points differ only in what they do with that
    /// context: `StartSpan` discards it, `StartAsCurrentSpan` installs it.
    /// It cannot be read back off the returned handle, because the unsampled
    /// path returns the process-wide no-op singleton whose `GetContext()` is
    /// invalid — and that is exactly the path ICP 0025 §3 contract 3 needs a
    /// real context for.
    ///
    /// @param started non-owning out-parameter, or `nullptr` to discard. When
    ///        non-null it is always written, including on the drop path.
    [[nodiscard]] SpanHandle StartSpanInternal(std::string_view name,
                                               const StartSpanOptions& opts,
                                               SpanContext* started) noexcept;

    internal::ISampler* m_sampler;
    internal::ISpanProcessor* m_processor;
    std::shared_ptr<const Resource> m_resource;
    internal::InstrumentationScope m_scope;
    SpanLimitOptions m_limits;
    internal::IDiagnosticsSink* m_diagnostics;
};

}  // namespace microtel::sdk
