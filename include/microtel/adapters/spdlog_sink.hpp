// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace microtel::adapters
{

/// @brief Map an spdlog level to an OTel `SeverityNumber` (Logs Data Model).
///
/// spdlog's six active levels map onto the base OTel severities; `off` and any
/// out-of-range value map to `Unspecified`.
[[nodiscard]] inline SeverityNumber ToSeverityNumber(spdlog::level::level_enum level) noexcept
{
    switch (level)
    {
        case spdlog::level::trace:
            return SeverityNumber::Trace;
        case spdlog::level::debug:
            return SeverityNumber::Debug;
        case spdlog::level::info:
            return SeverityNumber::Info;
        case spdlog::level::warn:
            return SeverityNumber::Warn;
        case spdlog::level::err:
            return SeverityNumber::Error;
        case spdlog::level::critical:
            return SeverityNumber::Fatal;
        default:
            return SeverityNumber::Unspecified;
    }
}

/// @brief An spdlog sink that forwards each log message to a microtel `Logger`
/// as an OTel `LogRecord`.
///
/// Bridges an application's existing spdlog logging into the OTLP logs pipeline:
/// register an instance on an `spdlog::logger` and every message it handles is
/// emitted through the microtel `Logger` obtained from `Provider::GetLogger`.
/// The message level becomes the `SeverityNumber`, the spdlog level name the
/// `severity_text`, the raw payload the `body`, and the event time the `time`.
///
/// The `Logger` is shared-owned so the sink keeps it alive. `SpdlogSinkMt`
/// (mutex-guarded) and `SpdlogSinkSt` (single-threaded) follow the usual spdlog
/// sink convention.
///
/// @threadsafety As per the `Mutex` template parameter, plus the microtel
///               `Logger::Emit` contract (itself thread-safe).
/// @see docs/logs-design.md §10
template <typename Mutex>
class BasicSpdlogSink final : public spdlog::sinks::base_sink<Mutex>
{
public:
    explicit BasicSpdlogSink(std::shared_ptr<microtel::Logger> logger) noexcept
        : m_logger(std::move(logger))
    {
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        if (m_logger == nullptr)
        {
            return;
        }
        microtel::LogRecord record;
        record.time = msg.time;
        record.severity_number = ToSeverityNumber(msg.level);
        const auto level_name = spdlog::level::to_string_view(msg.level);
        record.severity_text.assign(level_name.data(), level_name.size());
        record.body = std::string{msg.payload.data(), msg.payload.size()};
        m_logger->Emit(std::move(record));
    }

    void flush_() override {}

private:
    std::shared_ptr<microtel::Logger> m_logger;
};

/// @brief Thread-safe spdlog bridge sink (a `std::mutex` guards each record).
using SpdlogSinkMt = BasicSpdlogSink<std::mutex>;

/// @brief Single-threaded spdlog bridge sink (no internal locking).
using SpdlogSinkSt = BasicSpdlogSink<spdlog::details::null_mutex>;

}  // namespace microtel::adapters
