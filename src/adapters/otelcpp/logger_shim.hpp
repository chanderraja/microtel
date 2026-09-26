// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/logger.hpp"
#include "microtel/provider.hpp"

#include "adapters/otelcpp/log_record_shim.hpp"
#include "adapters/otelcpp/shim_options.hpp"

#include <memory>
#include <string>
#include <utility>

#include <opentelemetry/common/key_value_iterable.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/logger_provider.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>

/// @file
/// `LoggerShim` / `LoggerProviderShim` — implement otel-cpp's `logs::Logger`
/// and `logs::LoggerProvider` (ABI v1) over `microtel::Logger` and
/// `microtel::Provider`. M17 L4.

namespace microtel::adapters::otelcpp
{

/// @brief An otel-cpp logger backed by a microtel logger.
///
/// `CreateLogRecord()` / `EmitLogRecord()` implement otel-cpp's
/// create-populate-emit contract: the record returned by this logger's
/// `CreateLogRecord()` must be the one passed back to its `EmitLogRecord()`.
/// `EmitLogRecord` downcasts on that contract with `static_cast` — the same
/// unchecked downcast the reference SDK's `Logger::EmitLogRecord` uses
/// (`sdk/src/logs/logger.cc`).
///
/// @threadsafety Thread-safe, matching `microtel::Logger`.
class LoggerShim final : public opentelemetry::logs::Logger
{
public:
    /// @param name   the resolved instrumentation-scope name (see
    ///               `LoggerProviderShim::GetLogger`). Returned by `GetName`.
    /// @param logger the microtel logger to forward emitted records to. Must
    ///               be non-null.
    /// @param options copied into every record `CreateLogRecord` returns.
    LoggerShim(std::string name,
               std::shared_ptr<microtel::Logger> logger,
               ShimOptions options = {}) noexcept
        : m_name{std::move(name)}, m_logger{std::move(logger)}, m_options{options}
    {
    }

    [[nodiscard]] const opentelemetry::nostd::string_view GetName() noexcept override
    {
        return m_name;
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>
    CreateLogRecord() noexcept override
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>{
            std::make_unique<LogRecordShim>(m_options)};
    }

    void EmitLogRecord(opentelemetry::nostd::unique_ptr<opentelemetry::logs::LogRecord>&&
                           log_record) noexcept override
    {
        if (!log_record)
        {
            return;
        }
        auto* shim = static_cast<LogRecordShim*>(log_record.get());
        m_logger->Emit(shim->ReleaseRecord());
    }

private:
    std::string m_name;
    std::shared_ptr<microtel::Logger> m_logger;
    ShimOptions m_options;
};

/// @brief An otel-cpp logger provider backed by a microtel provider.
///
/// `schema_url` and `attributes` have no surface on
/// `microtel::Provider::GetLogger` and are dropped, matching the
/// `TracerProviderShim` precedent for `schema_url`.
///
/// @threadsafety Thread-safe, matching `microtel::Provider`.
class LoggerProviderShim final : public opentelemetry::logs::LoggerProvider
{
public:
    /// @param provider the microtel provider to adapt. Must be non-null.
    /// @param options  copied into every logger this provider hands out.
    explicit LoggerProviderShim(std::shared_ptr<microtel::Provider> provider,
                                ShimOptions options = {}) noexcept
        : m_provider{std::move(provider)}, m_options{options}
    {
    }

    /// @brief Resolve the instrumentation-scope name and forward to
    ///        `Provider::GetLogger`.
    ///
    /// otel-cpp's `logger_name` is the parameter every single-argument call
    /// site (`GetLogger("my.lib")`) actually passes; `name` only overrides it
    /// when a caller wants the instrumentation-scope name to differ from the
    /// logger identity. The reference SDK resolves this the same way:
    /// `if (name.empty()) name = logger_name;`
    /// (`sdk/src/logs/logger_provider.cc`, citing the Logs Data Model's
    /// InstrumentationScope field). Getting this backwards would silently
    /// scope every ordinary call under an empty name.
    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> GetLogger(
        opentelemetry::nostd::string_view logger_name,
        opentelemetry::nostd::string_view name = "",
        opentelemetry::nostd::string_view version = "",
        opentelemetry::nostd::string_view /*schema_url*/ = "",
        const otel_common::KeyValueIterable& /*attributes*/ =
            otel_common::NoopKeyValueIterable()) override
    {
        const opentelemetry::nostd::string_view resolved_name = name.empty() ? logger_name : name;
        auto logger = m_provider->GetLogger({resolved_name.data(), resolved_name.size()},
                                            {version.data(), version.size()});
        return opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger>{
            std::make_shared<LoggerShim>(std::string{resolved_name.data(), resolved_name.size()},
                                         std::move(logger),
                                         m_options)};
    }

private:
    std::shared_ptr<microtel::Provider> m_provider;
    ShimOptions m_options;
};

/// @brief Build an otel-cpp logger provider over a microtel provider, ready
///        for `opentelemetry::logs::Provider::SetLoggerProvider`.
///
/// @param options copied into the provider shim and every logger and log
///                record it creates (ICP 0033). They limit log attributes,
///                not the log body.
[[nodiscard]] inline opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>
MakeLoggerProvider(std::shared_ptr<microtel::Provider> provider, ShimOptions options = {})
{
    return opentelemetry::nostd::shared_ptr<opentelemetry::logs::LoggerProvider>{
        std::make_shared<LoggerProviderShim>(std::move(provider), options)};
}

}  // namespace microtel::adapters::otelcpp
