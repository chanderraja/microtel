// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/provider.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace microtel::testing
{

/// @brief Fake `IDiagnosticsSink` for assertions in tests.
///
/// Stores counters as plain `uint64_t` (not atomics — tests are
/// single-threaded per `tests/integration/README.md`). Exposes them
/// directly so test code can `EXPECT_EQ(sink.drop_counters[...], N)`.
class FakeDiagnosticsSink : public internal::IDiagnosticsSink
{
public:
    std::array<std::uint64_t, microtel::kDropReasonCount> drop_counters{};
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_failed = 0;
    std::uint64_t queue_depth_now = 0;

    std::optional<std::chrono::system_clock::time_point> last_error_time;
    std::string last_error_message;
    microtel::ConnectionState connection_state = microtel::ConnectionState::Disconnected;

    void RecordDrop(microtel::DropReason reason, std::uint64_t n = 1) noexcept override
    {
        drop_counters[static_cast<std::size_t>(reason)] += n;
    }

    void RecordBatchSent() noexcept override
    {
        ++batches_sent;
    }

    void RecordBatchFailed(const microtel::Error& err) noexcept override
    {
        ++batches_failed;
        last_error_time = std::chrono::system_clock::now();
        last_error_message = err.message;
    }

    void SetQueueDepth(std::uint64_t depth) noexcept override
    {
        queue_depth_now = depth;
    }

    void SetConnectionState(microtel::ConnectionState state) noexcept override
    {
        connection_state = state;
    }

    [[nodiscard]] microtel::HealthSnapshot Snapshot() const noexcept override
    {
        microtel::HealthSnapshot snap;
        snap.drop_counters = drop_counters;
        snap.batches_sent = batches_sent;
        snap.batches_failed = batches_failed;
        snap.queue_depth_now = queue_depth_now;
        snap.last_error_time = last_error_time;
        snap.last_error_message = last_error_message;
        snap.connection_state = connection_state;
        return snap;
    }
};

}  // namespace microtel::testing
