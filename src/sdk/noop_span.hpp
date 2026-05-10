// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/span.hpp"

namespace microtel::sdk
{

/// @brief Returns a `SpanHandle` backed by the static `NoopSpan` singleton.
///
/// The deleter is null — `unique_ptr` will not call delete. Safe to call
/// from any thread; the singleton is immutable and has process lifetime.
[[nodiscard]] SpanHandle MakeNoopHandle() noexcept;

}  // namespace microtel::sdk
