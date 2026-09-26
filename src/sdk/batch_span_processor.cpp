// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/batch_span_processor.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::sdk
{

namespace
{

/// Per-record overhead the estimate charges for the fields that are not owned
/// buffers: trace and span ids, the parent context, two timestamps, kind,
/// status, and the protobuf framing around them.
constexpr std::size_t kRecordFixedBytes = 64;
/// Per-event overhead: timestamp plus framing.
constexpr std::size_t kEventFixedBytes = 16;
/// Per-link overhead: the linked trace id, span id, and flags, plus framing.
constexpr std::size_t kLinkFixedBytes = 32;
/// What a scalar attribute value costs — the widest of bool, int64, double.
constexpr std::size_t kScalarValueBytes = 8;

[[nodiscard]] std::size_t ValueBytes(const AttributeValue& value) noexcept
{
    if (const auto* const s = std::get_if<std::string>(&value))
    {
        return s->size();
    }
    if (const auto* const strings = std::get_if<std::vector<std::string>>(&value))
    {
        std::size_t total = 0;
        for (const auto& s : *strings)
        {
            total += s.size();
        }
        return total;
    }
    if (const auto* const bools = std::get_if<std::vector<bool>>(&value))
    {
        return bools->size();
    }
    if (const auto* const ints = std::get_if<std::vector<std::int64_t>>(&value))
    {
        return ints->size() * sizeof(std::int64_t);
    }
    if (const auto* const doubles = std::get_if<std::vector<double>>(&value))
    {
        return doubles->size() * sizeof(double);
    }
    return kScalarValueBytes;
}

[[nodiscard]] std::size_t AttributesBytes(const std::vector<KeyValue>& attributes) noexcept
{
    std::size_t total = 0;
    for (const auto& kv : attributes)
    {
        total += kv.key.size() + ValueBytes(kv.value);
    }
    return total;
}

}  // namespace

std::size_t EstimateRecordBytes(const internal::SpanRecord& record) noexcept
{
    std::size_t total = kRecordFixedBytes + record.name.size() + record.status_description.size();
    total += AttributesBytes(record.attributes);
    for (const auto& event : record.events)
    {
        total += kEventFixedBytes + event.name.size() + AttributesBytes(event.attributes);
    }
    for (const auto& link : record.links)
    {
        total += kLinkFixedBytes + AttributesBytes(link.attributes);
    }
    return total;
}

BatchSpanProcessor::BatchSpanProcessor(internal::IExporter* exporter,
                                       std::shared_ptr<const Resource> resource,
                                       BatchOptions opts,
                                       std::uint32_t max_record_bytes,
                                       std::uint64_t max_total_queue_bytes,
                                       internal::IDiagnosticsSink* diag,
                                       internal::IBatchGroupExporter* group_exporter) noexcept
    : m_exporter(exporter),
      m_resource(std::move(resource)),
      m_opts(opts),
      m_max_record_bytes(max_record_bytes),
      m_max_total_queue_bytes(max_total_queue_bytes),
      m_diag(diag),
      m_group_exporter(group_exporter),
      m_worker([this] { WorkerLoop(); })
{
}

constexpr auto kBspDestructorTimeout = std::chrono::milliseconds(5000);

BatchSpanProcessor::~BatchSpanProcessor() noexcept
{
    (void)Shutdown(kBspDestructorTimeout);
    JoinWorker();
}

void BatchSpanProcessor::JoinWorker() noexcept
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void BatchSpanProcessor::OnStart(microtel::Span& /*span*/,
                                 const microtel::Context& /*parent*/) noexcept
{
    // No-op — v1 has no in-process span enrichment hooks.
}

void BatchSpanProcessor::OnEnd(internal::SpanRecord&& record,
                               const internal::InstrumentationScope& scope) noexcept
{
    (void)Enqueue(std::move(record), scope);
}

bool BatchSpanProcessor::Enqueue(internal::SpanRecord&& record,
                                 const internal::InstrumentationScope& scope) noexcept
{
    // Measured before the lock: the scan is over the caller's own record and
    // owes nothing to the queue, and holding `m_mu` across it would serialise
    // every other tracer thread behind one span's attribute list.
    const std::size_t record_bytes = EstimateRecordBytes(record);

    const std::scoped_lock lock{m_mu};
    if (m_shutdown)
    {
        RecordDropped(DropReason::PostShutdown);
        return false;
    }
    if (record_bytes > m_max_record_bytes)
    {
        // Refused before it is queued, so an oversized record never occupies
        // the queue it would otherwise dominate (issue #181, spec §5.5). The
        // limit is a ceiling the record may reach: only `>` drops.
        RecordDropped(DropReason::RecordTooLarge);
        return false;
    }
    if (!MakeRoomFor(record_bytes))
    {
        return false;
    }
    m_queue_bytes += record_bytes;
    m_queue.push_back(
        QueuedSpan{.record = std::move(record), .scope = scope, .bytes = record_bytes});
    if (m_queue.size() >= m_opts.max_export_batch_size)
    {
        m_cv.notify_one();
    }
    return true;
}

bool BatchSpanProcessor::MakeRoomFor(std::size_t record_bytes) noexcept
{
    // Two caps, one queue: `max_queue_size` counts records and
    // `max_total_queue_bytes` counts their estimated bytes (issue #181, spec
    // §5.5). Whichever fills first refuses the record, and both report it as
    // `QueueFull` — the queue is full, and a new `DropReason` would be an ICP
    // (`docs/interfaces.md` §3.5) for a distinction an operator reads the same
    // way. The byte cap can need more than one eviction, so this loops where
    // the count cap alone never had to.
    while (m_queue.size() >= m_opts.max_queue_size ||
           m_queue_bytes + record_bytes > m_max_total_queue_bytes)
    {
        // One span is lost per turn of the loop; the policy chooses which one.
        RecordDropped(DropReason::QueueFull);
        if (m_opts.drop_policy != DropPolicy::DropOldest)
        {
            return false;  // DropNewest: discard incoming record
        }
        if (m_queue.empty())
        {
            // A record larger than the whole budget: there is nothing left to
            // evict, so evicting more cannot help and the incoming record is
            // the one that goes. Without this the loop would spin forever.
            return false;
        }
        m_queue_bytes -= m_queue.front().bytes;
        m_queue.pop_front();
    }
    return true;
}

void BatchSpanProcessor::SetOptions(const BatchOptions& opts) noexcept
{
    const std::scoped_lock lock{m_mu};
    m_opts = opts;
    m_cv.notify_one();
}

microtel::Status BatchSpanProcessor::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock{m_mu};
    if (m_shutdown)
    {
        return microtel::Status::AlreadyShutDown;
    }
    const std::size_t target = ++m_flush_seq;
    m_cv.notify_one();
    const bool done =
        m_flush_cv.wait_for(lock, timeout, [this, target] { return m_flush_done_seq >= target; });
    return done ? microtel::Status::Completed : microtel::Status::TimedOut;
}

bool BatchSpanProcessor::JoinWithTimeout(std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_worker.joinable())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        m_worker.join();
    }
    return true;
}

microtel::Status BatchSpanProcessor::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    {
        const std::scoped_lock lock{m_mu};
        if (m_shutdown)
        {
            return microtel::Status::AlreadyShutDown;
        }
        m_shutdown = true;
        m_cv.notify_one();
    }
    return JoinWithTimeout(timeout) ? microtel::Status::Completed : microtel::Status::TimedOut;
}

BatchSpanProcessor::WakeResult BatchSpanProcessor::WaitAndCollect() noexcept
{
    std::unique_lock<std::mutex> lock{m_mu};
    m_cv.wait_for(lock,
                  m_opts.schedule_delay,
                  [this]
                  {
                      return m_shutdown || m_flush_seq > m_flush_done_seq ||
                             m_queue.size() >= m_opts.max_export_batch_size;
                  });

    const std::size_t count =
        std::min(m_queue.size(), static_cast<std::size_t>(m_opts.max_export_batch_size));
    std::vector<QueuedSpan> batch;
    batch.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        m_queue_bytes -= m_queue.front().bytes;
        batch.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
    }

    std::size_t pending_flush_seq = 0;
    if (m_flush_seq > m_flush_done_seq && m_queue.empty())
    {
        pending_flush_seq = m_flush_seq;
    }

    return {
        .batch = std::move(batch),
        .done = m_shutdown && m_queue.empty(),
        .pending_flush_seq = pending_flush_seq,
    };
}

void BatchSpanProcessor::WorkerLoop() noexcept
{
    while (true)
    {
        auto [batch, done, pending_flush_seq] = WaitAndCollect();
        if (!batch.empty())
        {
            ExportBatch(std::move(batch));
        }
        if (pending_flush_seq > 0)
        {
            const std::scoped_lock lock{m_mu};
            m_flush_done_seq = pending_flush_seq;
            m_flush_cv.notify_all();
        }
        if (done)
        {
            break;
        }
    }
}

void BatchSpanProcessor::RecordDropped(DropReason reason) noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->RecordDrop(reason);
    }
}

void BatchSpanProcessor::ExportBatch(std::vector<QueuedSpan> batch) noexcept
{
    // Group records by (Resource, scope), preserving first-seen order, into
    // one BatchHandle per group — ICP 0023, and design §3.6 for the Resource.
    // A null record Resource is the processor's own; a leaf Resource is
    // compared by pointer, since the leaf receiver shares one object per leaf.
    struct Group
    {
        const Resource* resource;
        std::shared_ptr<const Resource> owner;
        internal::InstrumentationScope scope;
        std::vector<internal::SpanRecord> records;
    };
    std::vector<Group> groups;
    for (auto& item : batch)
    {
        const std::shared_ptr<const Resource>& owner =
            item.record.resource != nullptr ? item.record.resource : m_resource;
        const Resource* const key = owner.get();
        const auto same_group = [&item, key](const Group& group)
        {
            return group.resource == key && group.scope.name == item.scope.name &&
                   group.scope.version == item.scope.version;
        };
        auto it = std::ranges::find_if(groups, same_group);
        if (it == groups.end())
        {
            // The one refcount taken per group, not per record.
            groups.push_back(Group{
                .resource = key, .owner = owner, .scope = std::move(item.scope), .records = {}});
            it = std::prev(groups.end());
        }
        it->records.push_back(std::move(item.record));
    }

    std::vector<internal::BatchHandle> handles;
    handles.reserve(groups.size());
    for (auto& group : groups)
    {
        handles.emplace_back(
            std::move(group.records), std::move(group.owner), std::move(group.scope));
    }
    if (m_group_exporter != nullptr)
    {
        m_group_exporter->ExportGroup(std::move(handles));
        return;
    }
    for (auto& handle : handles)
    {
        (void)m_exporter->Export(std::move(handle));
    }
}

}  // namespace microtel::sdk
