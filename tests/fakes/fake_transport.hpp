// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Scriptable `ITransport` for codec + exporter integration tests.
///
/// Distinct from `MockTransport` (which records call counts only):
/// `FakeTransport` retains every `RequestSpec` it received and serves
/// scripted `TransportResult`s in FIFO order. Tests that drive
/// retry / GOAWAY / partial-success scenarios script a sequence of
/// responses and the codec sees them in order.
///
/// No real socket, no thread. The future returned from `Send` is
/// resolved synchronously inside `Send` from the head of
/// `scripted_responses` (or `default_response` if the queue is empty).
class FakeTransport : public internal::ITransport
{
public:
    // --- Recording ---
    struct ConnectCall
    {
        internal::ConnectOptions opts;
    };

    std::vector<ConnectCall> connect_calls;
    /// Note: RequestSpec is move-only-ish (contains a span<const byte>
    /// borrow + a header vector). We store specs by value.
    std::vector<internal::RequestSpec> sent_specs;
    int cancel_call_count = 0;
    int close_call_count = 0;

    // --- Configurable ---
    microtel::Expected<void, microtel::Error> connect_result{};
    microtel::ConnectionState state = microtel::ConnectionState::Connected;
    microtel::Status close_result = microtel::Status::Completed;

    /// FIFO of scripted Send results. Each `Send` pops the front; falls
    /// back to `default_response` when empty.
    std::deque<internal::TransportResult> scripted_responses;
    /// Default-constructed (`success=false`); test sets `default_response.success = true`
    /// to opt in to a successful default.
    internal::TransportResult default_response{};

    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& opts) override
    {
        connect_calls.push_back(ConnectCall{opts});
        return connect_result;
    }

    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec spec) noexcept override
    {
        sent_specs.push_back(std::move(spec));
        std::promise<internal::TransportResult> p;
        std::future<internal::TransportResult> f = p.get_future();
        if (!scripted_responses.empty())
        {
            p.set_value(std::move(scripted_responses.front()));
            scripted_responses.pop_front();
        }
        else
        {
            p.set_value(default_response);
        }
        return internal::RequestHandle{static_cast<std::uint64_t>(sent_specs.size()), std::move(f)};
    }

    void Cancel(const internal::RequestHandle& /*handle*/) noexcept override
    {
        ++cancel_call_count;
    }

    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override
    {
        return state;
    }

    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++close_call_count;
        return close_result;
    }
};

}  // namespace microtel::testing
