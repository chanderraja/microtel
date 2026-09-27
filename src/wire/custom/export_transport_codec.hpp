// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/export_transport.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace microtel::wire
{

/// @brief The provider-wide side of an application's `ExportTransport`
///        (ICP 0036): ownership, the shutdown deadline, `Cancel`, and the
///        connection state.
///
/// One per `Provider`, shared by the one `ExportTransportCodec` each enabled
/// signal has. It owns the application's transport, so it must outlive every
/// codec and exporter worker that reaches it; `SdkProvider` declares it before
/// them.
///
/// **Connection state** means "sends are succeeding", not "the peer is
/// reachable": `Disconnected` until the first `Success`, `Connected` after
/// one, `Reconnecting` after a failure that follows a success, and `Closed`
/// after `MarkClosed`. On a fire-and-forget link `Connected` says only that
/// requests are leaving this host.
///
/// @threadsafety Thread-safe. `Send` may be called from up to three exporter
///   workers at once; the other methods from any thread.
class ExportTransportChannel final
{
public:
    /// @param transport the application's transport; ownership moves in.
    ///        Must be non-null.
    /// @param clock steady clock for deadlines, or nullptr for
    ///        `std::chrono::steady_clock`. Borrowed; must outlive this.
    explicit ExportTransportChannel(std::unique_ptr<ExportTransport> transport,
                                    internal::ISteadyClock* clock = nullptr) noexcept;
    ~ExportTransportChannel() noexcept = default;

    ExportTransportChannel(const ExportTransportChannel&) = delete;
    ExportTransportChannel& operator=(const ExportTransportChannel&) = delete;
    ExportTransportChannel(ExportTransportChannel&&) = delete;
    ExportTransportChannel& operator=(ExportTransportChannel&&) = delete;

    /// @brief Hand one request to the transport and map its answer.
    ///
    /// The deadline is now plus @p per_export, clamped to the shutdown
    /// deadline once `BeginShutdown` has run. After `CancelInFlight`, or once
    /// the shutdown deadline has passed, the transport is not called and the
    /// request is a non-retryable failure. A throw from the transport is
    /// contained here and becomes a non-retryable `InternalFailure`.
    ///
    /// @throws std::bad_alloc only, composing an error message.
    ///
    /// @param signal which request the bytes are.
    /// @param bytes non-empty; borrowed for the call.
    /// @param per_export the per-export timeout.
    [[nodiscard]] internal::WireResult Send(ExportSignal signal,
                                            std::span<const std::byte> bytes,
                                            std::chrono::milliseconds per_export);

    /// @brief Record that `Provider::Shutdown` began, with @p timeout to run.
    ///        Later deadlines are clamped to now plus @p timeout. Idempotent:
    ///        the first call fixes the deadline.
    void BeginShutdown(std::chrono::milliseconds timeout) noexcept;

    /// @brief An exporter's shutdown wait expired: call `ExportTransport::
    ///        Cancel` if a `Send` is in flight, at most once per channel, and
    ///        refuse every later `Send`.
    void CancelInFlight() noexcept;

    /// @brief `Provider::Shutdown` has finished: the state is `Closed` from now on.
    void MarkClosed() noexcept;

    /// @brief The connection state, as the class comment defines it.
    [[nodiscard]] ConnectionState State() const noexcept;

private:
    /// @brief The transport's answer mapped, or the contained exception's failure.
    [[nodiscard]] internal::WireResult InvokeTransport(const ExportRequest& request);
    /// @brief Enter a `Send`: false when cancelled or past the shutdown deadline.
    [[nodiscard]] bool Enter(internal::TimePointSteady now) noexcept;
    void Leave() noexcept;
    void NoteOutcome(bool success) noexcept;
    [[nodiscard]] internal::TimePointSteady Now() const noexcept;

    std::unique_ptr<ExportTransport> m_transport;
    internal::ISteadyClock* m_clock;
    /// Guards the three fields below, so that `CancelInFlight` sees an
    /// in-flight count that no `Send` can slip past between the check and the
    /// flag. A leaf lock: nothing else is taken under it, and the transport is
    /// never called while it is held.
    std::mutex m_mu;
    std::uint32_t m_in_flight = 0;
    bool m_cancelled = false;
    std::optional<internal::TimePointSteady> m_shutdown_deadline;
    std::atomic<ConnectionState> m_state{ConnectionState::Disconnected};
};

/// @brief The `IWireCodec` for one signal of a Provider built with
///        `SdkBuilder::WithExportTransport` (ICP 0036).
///
/// A third `IWireCodec` implementation beside HTTP and gRPC. It adds no
/// framing, headers or compression: it hands the encoded request to the
/// shared `ExportTransportChannel` and maps the `SendResult` back.
///
/// - An empty payload (an encode that failed to allocate) is a retryable
///   failure and never reaches the transport.
/// - With @p max_request_bytes set, a request larger than it is still sent
///   whole, with a `Warn` rate-limited to one a minute per codec.
///
/// @threadsafety Not thread-safe, like every `IWireCodec`: one exporter worker.
class ExportTransportCodec final : public internal::IWireCodec
{
public:
    /// @param channel borrowed; must outlive the codec.
    /// @param signal the signal this codec's exporter sends.
    /// @param max_request_bytes the size above which a request is logged; 0
    ///        for no check (metrics and logs).
    /// @param clock for the Warn's rate limit, or nullptr for
    ///        `std::chrono::steady_clock`. Borrowed.
    ExportTransportCodec(ExportTransportChannel* channel,
                         ExportSignal signal,
                         std::uint32_t max_request_bytes = 0,
                         internal::ISteadyClock* clock = nullptr) noexcept;

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& payload,
                                            std::chrono::milliseconds deadline) override;

private:
    void WarnIfOversized(std::size_t size) noexcept;

    ExportTransportChannel* m_channel;
    ExportSignal m_signal;
    std::uint32_t m_max_request_bytes;
    internal::ISteadyClock* m_clock;
    std::optional<internal::TimePointSteady> m_last_oversize_warn;
};

}  // namespace microtel::wire
