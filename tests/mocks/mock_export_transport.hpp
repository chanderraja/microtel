// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/export_transport.hpp"

#include <atomic>

namespace microtel::testing
{

/// @brief Dumb mock for the public `microtel::ExportTransport` (ICP 0036).
///
/// Returns `result_to_return` from every `Send`. `Cancel` only records the
/// call: it is pure virtual, so the mock has to define it, and a mock that
/// did anything more would be a fake.
class MockExportTransport : public ExportTransport
{
public:
    /// @brief Returned from every `Send`. Default-constructed it is a
    /// non-retryable failure; tests opt in to success.
    SendResult result_to_return{};

    // Atomic: the exporter worker calls Send while the test thread reads.
    std::atomic<int> send_call_count{0};
    std::atomic<int> cancel_call_count{0};

    [[nodiscard]] SendResult Send(const ExportRequest& /*request*/) override
    {
        ++send_call_count;
        return result_to_return;
    }

    void Cancel() noexcept override
    {
        ++cancel_call_count;
    }
};

}  // namespace microtel::testing
