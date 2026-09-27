// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/custom/export_transport_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/log_sink.hpp"

#include "common/internal_log.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::wire
{

namespace
{

/// Prefix of the error a contained throw becomes, as `CallbackAuthProvider`
/// words its own.
constexpr std::string_view kThrewPrefix = "ExportTransport::Send threw: ";

/// One oversized-request Warn per codec per interval.
constexpr auto kOversizeWarnInterval = std::chrono::minutes(1);

[[nodiscard]] internal::WireResult Failure(bool retryable, Error::Kind kind, std::string message)
{
    internal::WireResult r;
    r.retryable = retryable;
    r.error = Error{.kind = kind, .message = std::move(message), .os_errno = 0};
    return r;
}

[[nodiscard]] std::string DefaultMessage(SendOutcome outcome)
{
    return outcome == SendOutcome::Retryable ? "export transport: retryable send failure"
                                             : "export transport: send failed";
}

/// `SendResult` → `WireResult`: ICP 0036 Decision 1, `docs/error-model.md` §7.3.
[[nodiscard]] internal::WireResult MapResult(SendResult result)
{
    if (result.outcome == SendOutcome::Success)
    {
        internal::WireResult r;
        r.success = true;
        r.partial_success_rejected = result.rejected;
        return r;
    }
    std::string message =
        result.message.empty() ? DefaultMessage(result.outcome) : std::move(result.message);
    auto r =
        Failure(result.outcome == SendOutcome::Retryable, Error::Kind::Network, std::move(message));
    if (r.retryable)
    {
        r.retry_after = result.retry_after;
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// ExportTransportChannel
// ---------------------------------------------------------------------------

ExportTransportChannel::ExportTransportChannel(std::unique_ptr<ExportTransport> transport,
                                               internal::ISteadyClock* clock) noexcept
    : m_transport(std::move(transport)), m_clock(clock)
{
}

internal::TimePointSteady ExportTransportChannel::Now() const noexcept
{
    return m_clock != nullptr ? m_clock->Now() : std::chrono::steady_clock::now();
}

bool ExportTransportChannel::Enter(internal::TimePointSteady now) noexcept
{
    const std::scoped_lock lock{m_mu};
    if (m_cancelled || (m_shutdown_deadline.has_value() && now >= *m_shutdown_deadline))
    {
        return false;
    }
    ++m_in_flight;
    return true;
}

void ExportTransportChannel::Leave() noexcept
{
    const std::scoped_lock lock{m_mu};
    --m_in_flight;
}

internal::WireResult ExportTransportChannel::Send(ExportSignal signal,
                                                  std::span<const std::byte> bytes,
                                                  std::chrono::milliseconds per_export)
{
    const internal::TimePointSteady now = Now();
    if (!Enter(now))
    {
        // Past the cancel point or the shutdown deadline: the request would
        // be handed a deadline already gone, and a transport told to stop.
        return Failure(false, Error::Kind::Network, "export transport: shut down");
    }
    internal::TimePointSteady deadline = now + per_export;
    {
        const std::scoped_lock lock{m_mu};
        if (m_shutdown_deadline.has_value())
        {
            deadline = std::min(deadline, *m_shutdown_deadline);
        }
    }
    // Left on every path, including a throw composing an error message, so a
    // later CancelInFlight never waits on a Send that is gone.
    class InFlight
    {
    public:
        explicit InFlight(ExportTransportChannel* channel) noexcept : m_channel(channel) {}
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        InFlight(InFlight&&) = delete;
        InFlight& operator=(InFlight&&) = delete;
        ~InFlight() noexcept
        {
            m_channel->Leave();
        }

    private:
        ExportTransportChannel* m_channel;
    } const in_flight{this};
    internal::WireResult result =
        InvokeTransport(ExportRequest{.signal = signal, .bytes = bytes, .deadline = deadline});
    NoteOutcome(result.success);
    return result;
}

internal::WireResult ExportTransportChannel::InvokeTransport(const ExportRequest& request)
{
    // The boundary ICP 0036 Decision 1 promises, shaped like
    // `CallbackAuthProvider::InvokeCallback`: a throw from application code
    // costs this one request, not the exporter's whole drain (issue #251).
    try
    {
        return MapResult(m_transport->Send(request));
    }
    catch (const std::exception& e)
    {
        return Failure(false, Error::Kind::InternalFailure, std::string{kThrewPrefix} + e.what());
    }
    // Not belt-and-braces: the exporter worker's handler is
    // `catch (const std::exception&)` and its loop is `noexcept`, so a non-std
    // throw that got that far would be std::terminate rather than one dropped
    // request.
    catch (...)
    {
        return Failure(
            false, Error::Kind::InternalFailure, std::string{kThrewPrefix} + "non-std exception");
    }
}

void ExportTransportChannel::NoteOutcome(bool success) noexcept
{
    ConnectionState current = m_state.load(std::memory_order_relaxed);
    ConnectionState next = current;
    while (true)
    {
        if (current == ConnectionState::Closed)
        {
            return;
        }
        if (success)
        {
            next = ConnectionState::Connected;
        }
        else if (current == ConnectionState::Connected)
        {
            next = ConnectionState::Reconnecting;
        }
        else
        {
            return;
        }
        if (m_state.compare_exchange_weak(current, next, std::memory_order_relaxed))
        {
            return;
        }
    }
}

void ExportTransportChannel::BeginShutdown(std::chrono::milliseconds timeout) noexcept
{
    const internal::TimePointSteady deadline = Now() + timeout;
    const std::scoped_lock lock{m_mu};
    if (!m_shutdown_deadline.has_value())
    {
        m_shutdown_deadline = deadline;
    }
}

void ExportTransportChannel::CancelInFlight() noexcept
{
    bool call_cancel = false;
    {
        const std::scoped_lock lock{m_mu};
        if (m_cancelled)
        {
            return;
        }
        m_cancelled = true;
        call_cancel = m_in_flight > 0;
    }
    // Outside the lock: Cancel is application code, and the Send it wakes
    // takes the lock on its way out.
    if (call_cancel)
    {
        m_transport->Cancel();
    }
}

void ExportTransportChannel::MarkClosed() noexcept
{
    m_state.store(ConnectionState::Closed, std::memory_order_relaxed);
}

ConnectionState ExportTransportChannel::State() const noexcept
{
    return m_state.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// ExportTransportCodec
// ---------------------------------------------------------------------------

ExportTransportCodec::ExportTransportCodec(ExportTransportChannel* channel,
                                           ExportSignal signal,
                                           std::uint32_t max_request_bytes,
                                           internal::ISteadyClock* clock) noexcept
    : m_channel(channel), m_signal(signal), m_max_request_bytes(max_request_bytes), m_clock(clock)
{
}

internal::WireResult ExportTransportCodec::Send(internal::EncodedPayload&& payload,
                                                std::chrono::milliseconds deadline)
{
    const internal::EncodedPayload owned = std::move(payload);
    if (owned.Size() == 0)
    {
        // An encode that failed to allocate its arena. The transport is
        // promised non-empty bytes; the retry re-encodes.
        return Failure(true, Error::Kind::InternalFailure, "export transport: empty encoding");
    }
    WarnIfOversized(owned.Size());
    return m_channel->Send(m_signal, owned.Bytes(), deadline);
}

void ExportTransportCodec::WarnIfOversized(std::size_t size) noexcept
{
    if (m_max_request_bytes == 0 || size <= m_max_request_bytes)
    {
        return;
    }
    const internal::TimePointSteady now =
        m_clock != nullptr ? m_clock->Now() : std::chrono::steady_clock::now();
    if (m_last_oversize_warn.has_value() && now - *m_last_oversize_warn < kOversizeWarnInterval)
    {
        return;
    }
    m_last_oversize_warn = now;
    try
    {
        internal::LogImpl(LogLevel::Warn,
                          "export transport: a " + std::to_string(size) +
                              "-byte trace request exceeds max_request_bytes (" +
                              std::to_string(m_max_request_bytes) +
                              ") and is sent whole; a single batch is never split - lower "
                              "BatchOptions::max_export_batch_size to bound it");
    }
    catch (const std::exception&)
    {
        // Composing the line could only fail to allocate; the request is
        // sent regardless and the Warn is advisory.
        return;
    }
}

}  // namespace microtel::wire
