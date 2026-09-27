// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

/// @file
/// An application-supplied export transport (ICP 0036). A `Provider` built
/// with `SdkBuilder::WithExportTransport` hands every encoded OTLP request to
/// an `ExportTransport` the application owns, instead of to microtel's HTTP/2
/// transport. The link below it — a UART, a CAN bus, a UDP socket, a file — and
/// its framing, fragmentation and acknowledgement are the application's.

namespace microtel
{

/// @brief Which OTLP service request an `ExportRequest` carries.
///
/// The three Export requests share field numbers, so a receiver cannot tell
/// them apart from the bytes. An application routing more than one signal
/// carries this value across its link itself.
enum class ExportSignal : std::uint8_t
{
    Traces = 0,   ///< an ExportTraceServiceRequest
    Metrics = 1,  ///< an ExportMetricsServiceRequest
    Logs = 2,     ///< an ExportLogsServiceRequest
};

/// @brief One encoded OTLP request, handed to `ExportTransport::Send`.
struct ExportRequest
{
    ExportSignal signal = ExportSignal::Traces;
    /// Uncompressed protobuf bytes of the signal's Export*ServiceRequest.
    /// Borrowed for the duration of `Send` only; copy them to keep them.
    /// Never empty. Identical, byte for byte, across retries of one request,
    /// so a transport may deduplicate on content.
    std::span<const std::byte> bytes;
    /// `Send` must return by this time: now plus `TimeoutOptions::per_export`,
    /// clamped to the shutdown deadline once `Provider::Shutdown` has begun.
    std::chrono::steady_clock::time_point deadline;
};

/// @brief What became of one `Send`.
enum class SendOutcome : std::uint8_t
{
    Success = 0,       ///< delivered, or handed to the link
    Retryable = 1,     ///< try again later; the retry engine decides when
    NonRetryable = 2,  ///< drop this request
};

/// @brief The result of one `ExportTransport::Send`.
struct SendResult
{
    SendOutcome outcome = SendOutcome::NonRetryable;
    /// Retryable only: the earliest time to retry, like HTTP Retry-After.
    std::optional<std::chrono::milliseconds> retry_after;
    /// Success only: items the far end rejected (OTLP partial success).
    std::uint32_t rejected = 0;
    /// Failures only: recorded, capped, as `HealthSnapshot::last_error_message`.
    std::string message;
};

/// @brief Carries encoded OTLP requests over a link the application owns.
///
/// Owned by the `Provider`, which destroys it after every exporter worker has
/// been joined.
///
/// **Cancellation — read this before implementing.** A `Send` that ignores
/// both its deadline and `Cancel` makes `Provider::Shutdown` wait for it, and
/// makes the `Provider` destructor block **forever**. A blocking socket or
/// UART write with no timeout and a `Cancel` that does nothing is exactly
/// that. Decide how a blocked `Send` is woken: `shutdown()` or close the fd,
/// set a flag the write loop checks, or bound every write by
/// `ExportRequest::deadline` (for a socket, `SO_SNDTIMEO` computed from it —
/// never zero, which means "no timeout"). `examples/leaf/udp_full_node.cpp`
/// shows the pattern:
///
/// @code
/// SendResult Send(const ExportRequest& req) override
/// {
///     if (m_cancelled.load()) { return {.outcome = SendOutcome::NonRetryable}; }
///     SetSendTimeout(m_fd, req.deadline);          // at least 1 ms
///     if (sendto(m_fd, req.bytes.data(), req.bytes.size(), ...) < 0) { ... }
///     return {.outcome = SendOutcome::Success};
/// }
/// void Cancel() noexcept override
/// {
///     m_cancelled.store(true);
///     (void)::shutdown(m_fd, SHUT_RDWR);           // wakes a blocked sendto
/// }
/// @endcode
///
/// **Exceptions.** `Send` may throw. The throw is contained at the boundary:
/// that request becomes a non-retryable failure carrying
/// `Error::Kind::InternalFailure`, and no other request is affected.
///
/// @threadsafety `Send` may be called concurrently from one exporter worker
///   per enabled signal (at most three), never concurrently for one signal.
///   With the default `ExportTransportOptions` (traces only) there is one
///   caller. `Cancel` may be called from any thread, concurrently with `Send`.
/// @see docs/icps/0036-custom-export-transport.md
class ExportTransport
{
public:
    virtual ~ExportTransport() noexcept = default;

    /// @brief Carry one request. Called on an exporter worker thread, never on
    ///        an application thread and never on the hot path.
    ///
    /// May block, until `request.deadline`. A slow `Send` stalls its own
    /// signal's pipeline only.
    ///
    /// @param request borrowed for the call; see `ExportRequest::bytes`.
    /// @return the outcome; see `SendOutcome`.
    [[nodiscard]] virtual SendResult Send(const ExportRequest& request) = 0;

    /// @brief Wake every `Send`, in flight or later, and make it return promptly.
    ///
    /// Called at most once, from the thread running `Provider::Shutdown`, if
    /// the shutdown timeout expires while a `Send` is in flight. Returning
    /// `NonRetryable` at once from every `Send` after it is the expected
    /// answer. Pure virtual on purpose: a transport that really cannot block
    /// writes an empty `Cancel` on purpose.
    virtual void Cancel() noexcept = 0;
};

/// @brief Options for `SdkBuilder::WithExportTransport`.
struct ExportTransportOptions
{
    /// Export spans through the transport. When false, the provider's sampler
    /// is replaced by always-off: spans are not recorded and nothing is sent.
    bool traces = true;
    /// Export metrics through the transport. When false, `GetMeter` returns a
    /// no-op meter and no metrics pipeline is built.
    bool metrics = false;
    /// Export logs through the transport. When false, `GetLogger` returns a
    /// no-op logger and no logs pipeline is built.
    bool logs = false;
    /// Cap, in encoded uncompressed OTLP bytes, on joining trace batches into
    /// one request. 0 means no cap. Coalescing only: one batch that alone
    /// exceeds it is still sent whole, with a rate-limited `Warn`. Keep it at
    /// or below the receiver's own limit (a concentrator's
    /// `max_payload_bytes`, 64 KiB by default) and, to avoid fragmenting, the
    /// link's frame size: for UDP, under 65,507 bytes.
    std::uint32_t max_request_bytes = 64U * 1024U;
};

}  // namespace microtel
