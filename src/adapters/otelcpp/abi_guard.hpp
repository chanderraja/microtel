// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <opentelemetry/version.h>

/// @file
/// Configuration guard for the source-only shim.
///
/// The shim compiles inside the *consumer's* build (ICP 0014), so the
/// opentelemetry-cpp configuration in play is theirs, not ours. Under a
/// configuration the shim does not support, the failure mode without these
/// guards is "cannot instantiate abstract class" forty lines into a template
/// instantiation — an afternoon of confusion instead of a one-line answer.

#if OPENTELEMETRY_ABI_VERSION_NO != 1
#error "The microtel otelcpp shim supports OPENTELEMETRY_ABI_VERSION_NO=1 only. \
ABI v2 adds virtual methods (sync gauges, Span::AddLink, ...) the shim does not \
implement. Build opentelemetry-cpp and the shim with OPENTELEMETRY_ABI_VERSION_NO=1, \
or see src/adapters/otelcpp/README.md and ICP 0014 for the configuration contract."
#endif

#ifdef OPENTELEMETRY_HAVE_METRICS_BOUND_INSTRUMENTS_PREVIEW
#error "The microtel otelcpp shim does not support \
OPENTELEMETRY_HAVE_METRICS_BOUND_INSTRUMENTS_PREVIEW. The preview adds pure-virtual \
Bind() methods to Counter and Histogram that the shim does not implement. Build \
without the preview macro, or see src/adapters/otelcpp/README.md."
#endif
