// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/leaf_receiver.hpp"

namespace microtel::sdk
{

/// @brief The receiver `Provider::GetLeafReceiver` returns when the
///        concentrator is not enabled or not compiled in (ICP 0034).
///
/// Every `Ingest` answers `IngestStatus::Disabled` and counts nothing — not a
/// drop reason and not a stat — the way the no-op logger drops a record.
///
/// @threadsafety Thread-safe: stateless.
class NoopLeafReceiver final : public microtel::LeafReceiver
{
public:
    [[nodiscard]] IngestResult Ingest(const IngestRequest& /*request*/) noexcept override
    {
        return IngestResult{.status = IngestStatus::Disabled};
    }

    [[nodiscard]] LeafReceiverStats Stats() const noexcept override
    {
        return {};
    }
};

}  // namespace microtel::sdk
