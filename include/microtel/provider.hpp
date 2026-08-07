// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace microtel
{
class Meter;
}  // namespace microtel

namespace microtel
{

/// @brief Connection state observable through `HealthSnapshot`.
enum class ConnectionState : std::uint8_t
{
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Reconnecting = 3,
    Closed = 4,
};

/// @brief Drop reasons mirrored from `docs/error-model.md` §3.
///
/// The enumerator order is part of the `HealthSnapshot::drop_counters` array
/// indexing contract. Adding a new entry is an ICP because every counter is
/// part of the public health surface.
enum class DropReason : std::uint8_t
{
    QueueFull = 0,
    RecordTooLarge = 1,
    SpanAttributeLimit = 2,
    SpanEventLimit = 3,
    SpanLinkLimit = 4,
    EventAttributeLimit = 5,
    LinkAttributeLimit = 6,
    AttributeValueTruncated = 7,
    PostShutdown = 8,
    ResponseTooLarge = 9,
    DecompressionTooLarge = 10,
    MalformedResponse = 11,
    PartialSuccessRejection = 12,
    NonRetryableFailure = 13,
    RetryableFailureRecovered = 14,
    RetryBudgetExhausted = 15,
    TransportBusy = 16,
    ConnectFailure = 17,
    ForceFlushTimeout = 18,
    ShutdownTimeout = 19,
    /// Attribute set exceeded the per-instrument cardinality limit; the
    /// measurement is folded into the `otel.metric.overflow` series, not lost
    /// (ICP 0008, `docs/metrics-design.md` §2).
    CardinalityOverflow = 20,
    /// Async instrument callback exceeded the per-collection deadline; its
    /// measurements were dropped for that cycle (ICP 0008).
    MetricCallbackTimeout = 21,
    /// NaN / ±Inf measurement dropped, as the OTel spec requires (ICP 0008).
    NonFiniteValue = 22,
    /// A `LogRecord`'s attribute set exceeded the per-record attribute limit;
    /// the surplus attributes were dropped and `dropped_attributes_count` was
    /// incremented on the record (ICP 0011, `docs/logs-design.md` §5).
    LogAttributeLimit = 23,
};

/// @brief The number of `DropReason` enumerators. Used to size the counter
/// array in `HealthSnapshot`.
inline constexpr std::size_t kDropReasonCount = 24;

/// @brief Snapshot of exporter health, returned by `Provider::GetExporterHealth`.
///
/// A consistent-at-a-moment view; counters are atomic reads, the last-error
/// fields are a borrowed view of the most-recent error slot (capped).
struct HealthSnapshot
{
    std::array<std::uint64_t, kDropReasonCount> drop_counters{};
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_failed = 0;
    std::uint64_t queue_depth_now = 0;
    std::optional<std::chrono::system_clock::time_point> last_error_time;
    std::string last_error_message;  ///< capped, redacted
    ConnectionState connection_state = ConnectionState::Disconnected;
};

/// @brief Process-level entry point for issuing tracers and managing the
/// export pipeline lifecycle.
///
/// Constructed by `SdkBuilder::Build()`. Application code typically holds the
/// returned `shared_ptr<Provider>` for the duration of the process and calls
/// `Shutdown()` before exit.
///
/// @threadsafety Thread-safe. All methods may be called from any thread.
/// @see docs/architecture.md §3.1, §3.2
/// @see docs/threading-model.md §6
class Provider
{
public:
    Provider() noexcept = default;
    virtual ~Provider() noexcept = default;

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    Provider(Provider&&) = delete;
    Provider& operator=(Provider&&) = delete;

    /// @brief Acquire a tracer for one instrumentation scope.
    ///
    /// The returned `shared_ptr<Tracer>` is owned jointly with the provider;
    /// it remains valid until both this call site and the provider are
    /// destroyed.
    ///
    /// @param name    instrumentation library name (e.g., `"my.component"`).
    /// @param version optional library version string.
    [[nodiscard]] virtual std::shared_ptr<Tracer> GetTracer(std::string_view name,
                                                            std::string_view version = {}) = 0;

    /// @brief Eagerly establish the export connection.
    ///
    /// Optional. If not called, the connection is established lazily on the
    /// first export. Returns a network-level outcome — DNS / TCP / TLS /
    /// HTTP/2 SETTINGS exchange. No telemetry is sent.
    ///
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual Expected<void, Error> Connect() = 0;

    /// @brief Flush queued spans up to `timeout`.
    ///
    /// Does not stop accepting new records. Returns `Status::Completed` if the
    /// queue drained and the in-flight batch finished within the timeout.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status ForceFlush(std::chrono::milliseconds timeout) noexcept = 0;

    /// @brief Stop accepting new records, drain queues, close the transport.
    ///
    /// Idempotent. Subsequent calls return `Status::AlreadyShutDown`. After
    /// this returns, all `End()` calls drop with `PostShutdown`.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;

    /// @brief Snapshot of exporter health for diagnostics.
    ///
    /// Returns a consistent-at-a-moment view of counters, queue depth, last
    /// error, and connection state.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual HealthSnapshot GetExporterHealth() const noexcept = 0;

    /// @brief Acquire (or create) a `Meter` for one instrumentation scope.
    ///
    /// Same `(name, version)` returns the cached instance; a new instance is
    /// created on the first call for each unique scope. `schema_url` is stored
    /// in the scope but does not affect caching in v1.
    ///
    /// @param name        instrumentation library name (e.g., `"my.component"`).
    /// @param version     optional library version string.
    /// @param schema_url  optional schema URL (stored; no v1 semantics).
    ///
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual std::shared_ptr<Meter> GetMeter(std::string_view name,
                                                          std::string_view version = {},
                                                          std::string_view schema_url = {}) = 0;
};

}  // namespace microtel
