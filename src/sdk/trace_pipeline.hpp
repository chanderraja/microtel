// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/processor.hpp"
#include "microtel/sampler.hpp"

#include "sdk/diagnostics_counters.hpp"

#include <memory>

namespace microtel::sdk
{

/// @brief The provider state a `Tracer` and its sampled `Span`s dereference.
///
/// `Provider::GetTracer` promises that the returned tracer stays valid until
/// both the call site and the provider are destroyed (issue #285). So the three
/// things `SdkTracer` and `SdkSpan` reach through raw pointers live here rather
/// than directly in `SdkProvider`, and the provider shares this object with
/// every tracer and sampled span it hands out. Whichever of them lets go last
/// frees it.
///
/// A tracer or span that outlives its provider therefore meets a pipeline that
/// is shut down but still allocated: `~SdkProvider` runs `Shutdown` before it
/// releases its reference, so every later span ends in the processor's
/// `PostShutdown` drop (`docs/threading-model.md` §8 guarantee 7). The
/// processor must not reach the exporter after `Shutdown`, since the exporter
/// is gone by then; `BatchSpanProcessor` drops under its own lock, and
/// `~SdkProvider` joins its worker before destroying the exporter.
///
/// Member order is teardown order in reverse: the processor, which borrows the
/// diagnostics sink, is destroyed before it.
///
/// @threadsafety Immutable after construction. What it points at carries its
///               own thread-safety contract.
struct TracePipeline
{
    /// Every drop counter of the provider, not only the trace side's: the
    /// exporters, the metric storage and the log processors borrow it too.
    std::unique_ptr<DiagnosticsCounters> diagnostics;
    /// Fixed for the pipeline's life; hot reload retunes its ratio in place
    /// (ICP 0026).
    SamplerHandle sampler;
    std::unique_ptr<internal::ISpanProcessor> processor;
};

}  // namespace microtel::sdk
