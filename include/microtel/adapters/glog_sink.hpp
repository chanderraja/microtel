// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/adapters/code_attributes.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <glog/logging.h>

namespace microtel::adapters
{

namespace glog_detail
{

/// @brief The parameter type of a one-argument function pointer type.
template <typename Fn>
struct FirstParam;

template <typename R, typename P>
struct FirstParam<R (*)(P)>
{
    using Type = P;
};

// glog's severity values. Stable since glog's first release and identical in
// 0.6 (plain `int` constants) and 0.7 (an unscoped enum).
inline constexpr int kInfo = 0;
inline constexpr int kWarning = 1;
inline constexpr int kError = 2;
inline constexpr int kFatal = 3;

/// @brief A glog `LogMessageTime` as a `system_clock` time point.
///
/// glog 0.7 exposes `when()`; 0.6 has only `timestamp()` (whole seconds) and
/// `usec()`. A template so the branch for the other version is discarded
/// rather than compiled.
template <typename Time>
[[nodiscard]] std::chrono::system_clock::time_point ToTimePoint(const Time& time) noexcept
{
    if constexpr (requires { time.when(); })
    {
        return time.when();
    }
    else
    {
        return std::chrono::system_clock::from_time_t(time.timestamp()) +
               std::chrono::microseconds{time.usec()};
    }
}

}  // namespace glog_detail

/// @brief glog's severity type.
///
/// `LogSeverity` is a global `int` typedef in glog 0.6 and the enum
/// `google::LogSeverity` in 0.7, so neither spelling compiles against both.
/// This takes it from the parameter of `google::GetLogSeverityName`, which
/// both versions declare.
using GlogSeverity = glog_detail::FirstParam<decltype(&google::GetLogSeverityName)>::Type;

/// @brief Map a glog severity to an OTel `SeverityNumber` (Logs Data Model).
///
/// | glog      | OTel `SeverityNumber` |
/// |-----------|-----------------------|
/// | `INFO`    | `Info` (9)            |
/// | `WARNING` | `Warn` (13)           |
/// | `ERROR`   | `Error` (17)          |
/// | `FATAL`   | `Fatal` (21)          |
///
/// `VLOG(n)` messages reach a `LogSink` as `INFO`: glog does not pass the
/// verbosity level to sinks, so it cannot be mapped onto `Debug`/`Trace`.
/// Anything outside glog's four severities maps to `Unspecified`.
[[nodiscard]] inline SeverityNumber FromGlogSeverity(GlogSeverity severity) noexcept
{
    switch (static_cast<int>(severity))
    {
        case glog_detail::kInfo:
            return SeverityNumber::Info;
        case glog_detail::kWarning:
            return SeverityNumber::Warn;
        case glog_detail::kError:
            return SeverityNumber::Error;
        case glog_detail::kFatal:
            return SeverityNumber::Fatal;
        default:
            return SeverityNumber::Unspecified;
    }
}

/// @brief A glog `LogSink` that forwards each message to a microtel `Logger`
/// as an OTel `LogRecord`.
///
/// Registers itself with glog (`google::AddLogSink`) on construction and
/// unregisters (`google::RemoveLogSink`) on destruction, so its registration
/// is exactly its lifetime. Every `LOG(...)` / `VLOG(...)` the process emits
/// while it exists is converted as follows:
///
/// | `LogRecord` field     | from                                           |
/// |-----------------------|------------------------------------------------|
/// | `severity_number`     | `FromGlogSeverity(severity)`                   |
/// | `severity_text`       | glog's severity name (`"INFO"`, `"WARNING"`, …)|
/// | `body`                | the message text, without glog's prefix        |
/// | `time`                | the message's glog timestamp (µs resolution)   |
/// | `code.file.path`      | the call site's `__FILE__`                     |
/// | `code.line.number`    | the call site's line                           |
///
/// glog does not report the calling function to sinks, so `code.function.name`
/// is never set. Trace correlation needs nothing here: `Logger::Emit` fills
/// `trace_id` / `span_id` from the calling thread's current context, and glog
/// calls `send` on the logging thread.
///
/// **Supported glog versions:** 0.6.x and 0.7.x (both declare the
/// `LogMessageTime` overload of `LogSink::send` this overrides).
///
/// **Lifetime:** destroy the sink before the provider that issued its
/// `Logger` is destroyed (see `Logger`). A `LOG(FATAL)` is emitted like any
/// other record, but glog aborts the process straight after the sinks run, so
/// it is only exported if the log pipeline exports synchronously.
///
/// @threadsafety Thread-safe. glog may call `send` from any thread, and
///               concurrently; the sink holds no mutable state of its own.
/// @see docs/logs-design.md §10
class GlogSink final : public google::LogSink
{
public:
    /// @brief Construct the sink and register it with glog.
    ///
    /// @param logger microtel logger records are emitted through. May be null,
    ///               in which case every message is dropped.
    explicit GlogSink(std::shared_ptr<microtel::Logger> logger) : m_logger(std::move(logger))
    {
        google::AddLogSink(this);
    }

    /// @brief Unregister from glog.
    ///
    /// `RemoveLogSink` waits for any `send` already running on this sink, so no
    /// call reaches it after the destructor returns.
    ~GlogSink() noexcept override
    {
        google::RemoveLogSink(this);
    }

    // glog registers the sink by address.
    GlogSink(const GlogSink&) = delete;
    GlogSink& operator=(const GlogSink&) = delete;
    GlogSink(GlogSink&&) = delete;
    GlogSink& operator=(GlogSink&&) = delete;

    // The deprecated `std::tm*` overload stays reachable, as it is on the base.
    using google::LogSink::send;

    /// @brief Convert one glog message and emit it.
    ///
    /// Called by glog for every message; not normally called directly.
    /// `noexcept`: glog calls sinks from `LogMessage`'s destructor, where an
    /// exception would terminate the process anyway.
    void send(GlogSeverity severity,
              const char* full_filename,
              const char* /*base_filename*/,
              int line,
              const google::LogMessageTime& time,
              const char* message,
              std::size_t message_len) noexcept override
    {
        if (m_logger == nullptr)
        {
            return;
        }
        LogRecord record;
        record.time = glog_detail::ToTimePoint(time);
        record.severity_number = FromGlogSeverity(severity);
        record.severity_text = google::GetLogSeverityName(severity);
        record.body = std::string{message, message_len};
        if (full_filename != nullptr)
        {
            record.attributes.push_back(
                KeyValue{.key = std::string{kCodeFilePath}, .value = std::string{full_filename}});
        }
        record.attributes.push_back(KeyValue{.key = std::string{kCodeLineNumber},
                                             .value = static_cast<std::int64_t>(line)});
        m_logger->Emit(std::move(record));
    }

private:
    std::shared_ptr<microtel::Logger> m_logger;
};

}  // namespace microtel::adapters
