// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"  // InstrumentationScope
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"

#include <memory>
#include <span>
#include <vector>

namespace microtel::internal
{

/// @brief A move-only owning batch of completed log records sharing one
/// `Resource` and one `InstrumentationScope`.
///
/// Constructed by the SDK's log processor; consumed by `ILogEncoder::Encode`.
/// Mirrors `BatchHandle` (trace) and `MetricBatchHandle` (metrics) in shape.
class LogBatchHandle
{
public:
    LogBatchHandle() noexcept = default;

    LogBatchHandle(std::vector<LogRecord> records,
                   std::shared_ptr<const Resource> resource,
                   InstrumentationScope scope) noexcept
        : m_records(std::move(records)), m_resource(std::move(resource)), m_scope(std::move(scope))
    {
    }

    LogBatchHandle(const LogBatchHandle&) = delete;
    LogBatchHandle& operator=(const LogBatchHandle&) = delete;
    LogBatchHandle(LogBatchHandle&&) noexcept = default;
    LogBatchHandle& operator=(LogBatchHandle&&) noexcept = default;
    ~LogBatchHandle() noexcept = default;

    [[nodiscard]] std::span<const LogRecord> Records() const noexcept
    {
        return {m_records.data(), m_records.size()};
    }

    [[nodiscard]] const Resource& ResourceRef() const noexcept
    {
        return *m_resource;
    }

    [[nodiscard]] const InstrumentationScope& Scope() const noexcept
    {
        return m_scope;
    }

private:
    std::vector<LogRecord> m_records;
    std::shared_ptr<const Resource> m_resource;
    InstrumentationScope m_scope;
};

}  // namespace microtel::internal
