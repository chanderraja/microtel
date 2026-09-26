// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/adapters/code_attributes.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <log4cxx/appenderskeleton.h>
#include <log4cxx/helpers/pool.h>
#include <log4cxx/helpers/transcoder.h>
#include <log4cxx/level.h>
#include <log4cxx/logstring.h>
#include <log4cxx/spi/location/locationinfo.h>
#include <log4cxx/spi/loggingevent.h>

namespace microtel::adapters
{

/// @brief Map a log4cxx level (`Level::toInt()`) to an OTel `SeverityNumber`.
///
/// | log4cxx | OTel `SeverityNumber` |
/// |---------|-----------------------|
/// | `TRACE` | `Trace` (1)           |
/// | `DEBUG` | `Debug` (5)           |
/// | `INFO`  | `Info` (9)            |
/// | `WARN`  | `Warn` (13)           |
/// | `ERROR` | `Error` (17)          |
/// | `FATAL` | `Fatal` (21)          |
/// | `OFF`   | `Unspecified` (0)     |
///
/// A custom level maps to the base severity of the highest standard level at
/// or below it (a level between `INFO` and `WARN` is `Info`), which is the Logs
/// Data Model's guidance for sources with more levels than it has ranges.
/// Anything below `DEBUG`, `ALL` included, is `Trace`.
[[nodiscard]] inline SeverityNumber FromLog4cxxLevel(int level) noexcept
{
    using log4cxx::Level;
    if (level < Level::DEBUG_INT)
    {
        return SeverityNumber::Trace;
    }
    if (level < Level::INFO_INT)
    {
        return SeverityNumber::Debug;
    }
    if (level < Level::WARN_INT)
    {
        return SeverityNumber::Info;
    }
    if (level < Level::ERROR_INT)
    {
        return SeverityNumber::Warn;
    }
    if (level < Level::FATAL_INT)
    {
        return SeverityNumber::Error;
    }
    if (level < Level::OFF_INT)
    {
        return SeverityNumber::Fatal;
    }
    return SeverityNumber::Unspecified;
}

/// @brief A log4cxx appender that forwards each logging event to a microtel
/// `Logger` as an OTel `LogRecord`.
///
/// Attach it to a log4cxx logger (`logger->addAppender(appender)`) and every
/// event that logger passes to its appenders is converted as follows:
///
/// | `LogRecord` field    | from                                               |
/// |----------------------|----------------------------------------------------|
/// | `severity_number`    | `FromLog4cxxLevel(level->toInt())`                 |
/// | `severity_text`      | the level name (`"WARN"`, …)                       |
/// | `body`               | the rendered message                               |
/// | `time`               | the event timestamp (µs resolution)                |
/// | `code.file.path`     | the call site's file, when location is known       |
/// | `code.line.number`   | the call site's line, when location is known       |
/// | `code.function.name` | `ns::Class::method` as log4cxx parses it, if it can|
/// | other attributes     | one string attribute per MDC entry, under its key  |
///
/// The log4cxx logger name is not recorded: the instrumentation scope is the
/// microtel `Logger` the appender was built with. Attach separate appenders,
/// with separately named `Logger`s, where the split matters. The NDC is not
/// mapped. Trace correlation needs nothing here: `Logger::Emit` fills
/// `trace_id` / `span_id` from the calling thread's current context, and
/// log4cxx calls `append` on the logging thread (use no `AsyncAppender` in
/// front of this one, or the context is lost).
///
/// Programmatic use only: the class is not registered with log4cxx's class
/// registry, so it cannot be named in a log4cxx XML or properties file.
///
/// **Supported log4cxx versions:** 1.1 and later (`std::shared_ptr`-based
/// appenders; built and tested against 1.1 and 1.8).
///
/// **Lifetime:** remove the appender (or `close()` it) before the provider
/// that issued its `Logger` is destroyed (see `Logger`).
///
/// @threadsafety Thread-safe. `AppenderSkeleton::doAppend` serialises calls
///               to `append`, and `close()` may race with it safely.
/// @see docs/logs-design.md §10
class Log4cxxAppender final : public log4cxx::AppenderSkeleton
{
public:
    /// @param logger microtel logger records are emitted through. May be null,
    ///               in which case every event is dropped.
    explicit Log4cxxAppender(std::shared_ptr<microtel::Logger> logger) : m_logger(std::move(logger))
    {
    }

    /// @brief Stop forwarding. Events that arrive afterwards are dropped.
    void close() override
    {
        m_closed.store(true, std::memory_order_release);
    }

    /// @brief No layout: the record carries structured fields, not a line.
    [[nodiscard]] bool requiresLayout() const override
    {
        return false;
    }

protected:
    /// @brief Convert one event and emit it. Called by `doAppend`.
    ///
    /// `noexcept`: an exception would otherwise escape into the application's
    /// logging call.
    void append(const log4cxx::spi::LoggingEventPtr& event,
                log4cxx::helpers::Pool& /*pool*/) noexcept override
    {
        if (m_logger == nullptr || event == nullptr || m_closed.load(std::memory_order_acquire))
        {
            return;
        }
        m_logger->Emit(MakeRecord(*event));
    }

private:
    [[nodiscard]] static std::string ToUtf8(const log4cxx::LogString& text)
    {
        LOG4CXX_ENCODE_CHAR(utf8, text);
        return utf8;
    }

    static void AddLocation(const log4cxx::spi::LocationInfo& location,
                            std::vector<KeyValue>& attributes)
    {
        // log4cxx reports an unknown location as line -1.
        if (location.getLineNumber() < 0)
        {
            return;
        }
        attributes.push_back(KeyValue{.key = std::string{kCodeFilePath},
                                      .value = std::string{location.getFileName()}});
        attributes.push_back(
            KeyValue{.key = std::string{kCodeLineNumber},
                     .value = static_cast<std::int64_t>(location.getLineNumber())});
        // log4cxx parses the method and class out of __PRETTY_FUNCTION__, and
        // reports neither for a function it cannot parse (one in an anonymous
        // namespace, for instance).
        const std::string method = location.getMethodName();
        if (method.empty())
        {
            return;
        }
        std::string function = location.getClassName();
        if (!function.empty())
        {
            function += "::";
        }
        function += method;
        attributes.push_back(
            KeyValue{.key = std::string{kCodeFunctionName}, .value = std::move(function)});
    }

    static void AddMdc(const log4cxx::spi::LoggingEvent& event, std::vector<KeyValue>& attributes)
    {
        for (const auto& key : event.getMDCKeySet())
        {
            log4cxx::LogString value;
            if (event.getMDC(key, value))
            {
                attributes.push_back(KeyValue{.key = ToUtf8(key), .value = ToUtf8(value)});
            }
        }
    }

    [[nodiscard]] static LogRecord MakeRecord(const log4cxx::spi::LoggingEvent& event)
    {
        LogRecord record;
        record.time = event.getChronoTimeStamp();
        const log4cxx::LevelPtr& level = event.getLevel();
        if (level != nullptr)
        {
            record.severity_number = FromLog4cxxLevel(level->toInt());
            log4cxx::LogString name;
            level->toString(name);
            record.severity_text = ToUtf8(name);
        }
        record.body = ToUtf8(event.getRenderedMessage());
        AddLocation(event.getLocationInformation(), record.attributes);
        AddMdc(event, record.attributes);
        return record;
    }

    std::shared_ptr<microtel::Logger> m_logger;
    std::atomic<bool> m_closed{false};
};

}  // namespace microtel::adapters
