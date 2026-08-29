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
    /// Owned copies of the payload bytes from each Send() call.
    /// Use this instead of sent_specs[i].payload — the span borrows from
    /// the caller's buffer which may not survive after Send() returns.
    std::vector<std::vector<std::byte>> sent_payloads;
    int cancel_call_count = 0;
    int close_call_count = 0;

    // --- Configurable ---
    microtel::Expected<void, microtel::Error> connect_result{};
    microtel::ConnectionState state = microtel::ConnectionState::Connected;
    microtel::Status close_result = microtel::Status::Completed;

    /// FIFO of scripted Send results. Each `Send` pops the front; falls
    /// back to `default_response` when empty.
    std::deque<internal::TransportResult> scripted_responses;
    /// When true, Send returns a handle whose promise is never fulfilled —
    /// the state a mid-connection drop used to leave in-flight requests in.
    bool abandon_promises = false;
    /// Holds the unfulfilled promises alive; letting them destruct would set
    /// broken_promise and the future would throw instead of blocking.
    std::vector<std::promise<internal::TransportResult>> abandoned;
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
        sent_payloads.emplace_back(spec.payload.begin(), spec.payload.end());
        sent_specs.push_back(std::move(spec));
        std::promise<internal::TransportResult> p;
        std::future<internal::TransportResult> f = p.get_future();
        if (abandon_promises)
        {
            // Reproduce a mid-connection drop: the promise is never fulfilled,
            // exactly as the transport's drop path left it before ICP 0018.
            // Keeping it alive (rather than letting ~promise set broken_promise)
            // is what makes the wait block rather than throw.
            abandoned.push_back(std::move(p));
            return internal::RequestHandle{static_cast<std::uint64_t>(sent_specs.size()),
                                           std::move(f)};
        }
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
