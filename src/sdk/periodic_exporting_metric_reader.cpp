// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/periodic_exporting_metric_reader.hpp"

#include "microtel/internal/metric_batch.hpp"

#include <exception>
#include <vector>

namespace microtel::sdk
{

PeriodicExportingMetricReader::PeriodicExportingMetricReader(
    internal::IMetricProducer& producer,
    internal::IMetricExporter& exporter,
    std::chrono::milliseconds interval,
    internal::AggregationTemporality temporality) noexcept
    : m_producer(producer), m_exporter(exporter), m_interval(interval), m_temporality(temporality)
{
    m_thread = std::thread{[this] { RunLoop(); }};
}

PeriodicExportingMetricReader::~PeriodicExportingMetricReader() noexcept
{
    if (!m_shut_down.load(std::memory_order_acquire))
    {
        (void)Shutdown(std::chrono::milliseconds{5'000});
    }
}

microtel::Status PeriodicExportingMetricReader::Collect(
    std::chrono::milliseconds /*timeout*/) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return microtel::Status::AlreadyShutDown;
    }
    return DoCollectExport();
}

microtel::Status PeriodicExportingMetricReader::ForceFlush(
    std::chrono::milliseconds timeout) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        return microtel::Status::AlreadyShutDown;
    }
    (void)DoCollectExport();
    return m_exporter.ForceFlush(timeout);
}

microtel::Status PeriodicExportingMetricReader::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    if (m_shut_down.exchange(true, std::memory_order_acq_rel))
    {
        return microtel::Status::AlreadyShutDown;
    }
    {
        const std::scoped_lock lk{m_mu};
        m_wake = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    return m_exporter.Shutdown(timeout);
}

void PeriodicExportingMetricReader::RunLoop() noexcept
{
    while (!m_shut_down.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lk{m_mu};
        m_cv.wait_for(lk, m_interval, [this] { return m_wake; });
        m_wake = false;
        lk.unlock();
        (void)DoCollectExport();
    }
}

microtel::Status PeriodicExportingMetricReader::DoCollectExport() noexcept
{
    const std::scoped_lock lk{m_collect_mu};
    std::vector<internal::MetricBatchHandle> handles;
    try
    {
        handles = m_producer.Collect(m_temporality);
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

}  // namespace microtel::sdk
