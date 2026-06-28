// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/synchronous_metric_reader.hpp"

#include "microtel/internal/metric_batch.hpp"

#include <exception>
#include <vector>

namespace microtel::sdk
{

microtel::Status SynchronousMetricReader::Collect(std::chrono::milliseconds /*timeout*/) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return microtel::Status::AlreadyShutDown;
    }

    std::vector<internal::MetricBatchHandle> handles;
    try
    {
        handles = m_producer.Collect();
    }
    catch (const std::exception&)
    {
        return microtel::Status::Failed;
    }

    for (auto& handle : handles)
    {
        if (m_exporter.Export(std::move(handle)) == internal::ExportResult::Failure)
        {
            return microtel::Status::Failed;
        }
    }
    return microtel::Status::Completed;
}

microtel::Status SynchronousMetricReader::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return microtel::Status::AlreadyShutDown;
    }
    return m_exporter.ForceFlush(timeout);
}

microtel::Status SynchronousMetricReader::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    if (m_shut_down.exchange(true, std::memory_order_acq_rel))
    {
        return microtel::Status::AlreadyShutDown;
    }
    return m_exporter.Shutdown(timeout);
}

}  // namespace microtel::sdk
