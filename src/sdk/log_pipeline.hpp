// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/log_record_processor.hpp"

#include "sdk/current_span_source.hpp"

#include <memory>

namespace microtel::sdk
{

/// @brief The provider state an `SdkLogger` dereferences on every `Emit`.
///
/// `Provider::GetLogger` hands out a `shared_ptr<Logger>`, which a caller
/// reads as "this keeps itself alive" (issue #417). So the three things
/// `SdkLogger` reaches through raw pointers live here rather than directly in
/// `SdkProvider`, and the provider shares this object with every logger it
/// hands out — the log-side twin of `TracePipeline` (issue #285). Whichever of
/// them lets go last frees it.
///
/// A logger that outlives its provider therefore meets a processor that is
/// shut down but still allocated: `~SdkProvider` runs `Shutdown` and joins the
/// processor's worker before it releases its reference, so every later record
/// drops in `OnEmit` as `PostShutdown` without reaching the exporter, which is
/// gone by then.
///
/// Member order is teardown order in reverse: the processor, which borrows the
/// diagnostics sink, is destroyed before it.
///
/// @threadsafety Immutable after construction. What it points at carries its
///               own thread-safety contract.
struct LogPipeline
{
    /// The provider's sink, held through an aliasing `shared_ptr` into its
    /// `TracePipeline`, so it lives as long as any logger does.
    std::shared_ptr<internal::IDiagnosticsSink> diagnostics;
    /// The trace-correlation seam (ICP 0025 §3). Stateless.
    CurrentSpanSource span_source;
    std::unique_ptr<internal::ILogRecordProcessor> processor;
};

}  // namespace microtel::sdk
