// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include "fakes/fake_span.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `microtel::Tracer` that records every `StartSpan` and hands
///        out a fresh recording `FakeSpan` per call.
///
/// The fake owns its spans; the returned `SpanHandle` borrows them with a
/// no-op deleter, so tests inspect `spans[i]` after the shim under test has
/// released its handle. `StartSpanOptions::attributes` is borrowed storage
/// and is copied at call time. Single-threaded use only.
class FakeTracer : public microtel::Tracer
{
public:
    struct RecordedStart
    {
        std::string name;
        std::vector<microtel::KeyValue> attributes;
        std::optional<microtel::SpanContext> parent;
        microtel::SpanKind kind = microtel::SpanKind::Internal;
        std::chrono::system_clock::time_point start_time;
    };

    std::vector<RecordedStart> starts;
    std::vector<std::unique_ptr<FakeSpan>> spans;

    [[nodiscard]] microtel::SpanHandle StartSpan(
        std::string_view name, const microtel::StartSpanOptions& opts) noexcept override
    {
        starts.push_back({.name = std::string{name},
                          .attributes = {opts.attributes.begin(), opts.attributes.end()},
                          .parent = opts.parent,
                          .kind = opts.kind,
                          .start_time = opts.start_time});
        spans.push_back(std::make_unique<FakeSpan>());
        return microtel::SpanHandle{spans.back().get(),
                                    microtel::internal::SpanDeleter{.deleter = nullptr}};
    }

    [[nodiscard]] microtel::SpanHandle StartAsCurrentSpan(
        std::string_view name, const microtel::StartSpanOptions& opts) noexcept override
    {
        return StartSpan(name, opts);
    }
};

}  // namespace microtel::testing
