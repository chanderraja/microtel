// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/leaf_receiver.hpp"

#include <atomic>

namespace microtel::testing
{

/// @brief No-op `microtel::LeafReceiver` that `FakeProvider` hands out
///        (ICP 0034).
///
/// Answers every `Ingest` with a default `IngestResult` — status `Disabled`,
/// the same answer the SDK's own no-op receiver gives — and counts the calls.
class FakeLeafReceiver : public microtel::LeafReceiver
{
public:
    std::atomic<int> ingest_call_count{0};

    [[nodiscard]] microtel::IngestResult Ingest(
        const microtel::IngestRequest& /*request*/) noexcept override
    {
        ++ingest_call_count;
        return {};
    }

    [[nodiscard]] microtel::LeafReceiverStats Stats() const noexcept override
    {
        return {};
    }
};

}  // namespace microtel::testing
