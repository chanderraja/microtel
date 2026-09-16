// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace microtel
{
class Span;
class Context;
}  // namespace microtel

namespace microtel::sdk
{

/// @brief Estimate the byte cost of one span record.
///
/// `max_record_bytes` has to be applied *before* the record is queued (issue
/// #181 names `BatchSpanProcessor::OnEnd` as its detection point), and at that
/// point nothing has encoded the record — so this is a documented estimate of
/// the memory the record occupies, not a wire measurement:
///
/// ```text
/// bytes = kRecordFixedBytes                        // ids, timestamps, kind, status
///       + name + status_description
///       + Σ attributes ( key + value )
///       + Σ events     ( kEventFixedBytes + name + Σ attributes( key + value ) )
///       + Σ links      ( kLinkFixedBytes  + Σ attributes( key + value ) )
/// ```
///
/// A value costs its string bytes, the sum of its elements' bytes for a string
/// array, its element count times the element width for a numeric or boolean
/// array, and `kScalarValueBytes` for a scalar. Per-field protobuf tags and
/// varint lengths are folded into the three fixed constants rather than
/// modelled individually: the limit is a memory budget, and an estimate a test
/// can reproduce is worth more here than one that tracks the encoder.
///
/// @param record the record about to be queued. Borrowed; not retained.
/// @return the estimated size in bytes; never zero.
[[nodiscard]] std::size_t EstimateRecordBytes(const internal::SpanRecord& record) noexcept;

/// @brief Asynchronous batching span processor.
///
/// Queues `SpanRecord`s from `OnEnd` and drains them on a dedicated worker
/// thread in batches of up to `max_export_batch_size` records.  Draining is
/// triggered by:
///   - the `schedule_delay` timer (periodic),
///   - the queue reaching `max_export_batch_size` (immediate wake), or
///   - `ForceFlush` / `Shutdown` (explicit signal).
///
/// On drain, the collected records are grouped by `(Resource,
/// InstrumentationScope)` into one `BatchHandle` per scope (ICP 0023), each
/// handed to the exporter, so spans from different tracers never share a
/// `ScopeSpans` entry on the wire.
///
/// `OnStart` is a no-op (v1 has no enrichment hooks).
///
/// **Lifetime.** The exporter and resource pointers are non-owning; the
/// caller keeps them alive for the processor's lifetime.
///
/// @threadsafety Thread-safe — `OnEnd` / `ForceFlush` / `Shutdown` may be
///               called concurrently from multiple threads.
class BatchSpanProcessor final : public internal::ISpanProcessor
{
public:
    /// @param exporter non-owning; must outlive the processor.
    /// @param resource shared with every batch this processor emits.
    /// @param opts queue capacity, batch size, schedule delay, drop policy.
    /// @param max_record_bytes ceiling on one record's `EstimateRecordBytes`;
    ///        a record above it is dropped in `OnEnd` and counted as
    ///        `RecordTooLarge`. From `MemoryLimitOptions::max_record_bytes`.
    /// @param max_total_queue_bytes ceiling on the summed `EstimateRecordBytes`
    ///        of everything queued. A record that would push the queue over it
    ///        is refused (or displaces the oldest, per `opts.drop_policy`) and
    ///        counted as `QueueFull` — the queue is full, by whichever of the
    ///        two budgets filled first. From
    ///        `MemoryLimitOptions::max_total_queue_bytes`. Scalars rather than
    ///        the whole options struct because these two are the only fields of
    ///        it this processor enforces.
    /// @param diag non-owning diagnostics sink, or `nullptr` to disable drop
    ///        accounting. Borrowed for the processor's lifetime.
    BatchSpanProcessor(
        internal::IExporter* exporter,
        std::shared_ptr<const Resource> resource,
        BatchOptions opts,
        std::uint32_t max_record_bytes = MemoryLimitOptions{}.max_record_bytes,
        std::uint64_t max_total_queue_bytes = MemoryLimitOptions{}.max_total_queue_bytes,
        internal::IDiagnosticsSink* diag = nullptr) noexcept;

    ~BatchSpanProcessor() noexcept override;

    BatchSpanProcessor(const BatchSpanProcessor&) = delete;
    BatchSpanProcessor& operator=(const BatchSpanProcessor&) = delete;
    BatchSpanProcessor(BatchSpanProcessor&&) = delete;
    BatchSpanProcessor& operator=(BatchSpanProcessor&&) = delete;

    void OnStart(microtel::Span& span, const microtel::Context& parent) noexcept override;
    void OnEnd(internal::SpanRecord&& record,
               const internal::InstrumentationScope& scope) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Retune the batching knobs while the processor runs (ICP 0026).
    ///
    /// Takes `m_mu`, assigns `m_opts`, notifies the worker. The notify is not
    /// only politeness: it makes the worker re-evaluate its predicate against
    /// the **new** `max_export_batch_size`, which can drain immediately if the
    /// queue already exceeds it. A shortened `schedule_delay` still costs at
    /// most one old-length tick, because `wait_for` fixed its deadline at
    /// entry and a notify whose predicate is false does not shorten it.
    ///
    /// `MemoryLimitOptions`'s two caps are **not** retunable and are not
    /// touched here; they come from a different options struct and stay
    /// immutable after construction.
    ///
    /// Validation belongs to the caller — `SdkProvider::SetBatchOptions`
    /// rejects an incoherent `opts` before reaching this.
    ///
    /// @param opts borrowed; copied under the lock. Not retained.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    void SetOptions(const BatchOptions& opts) noexcept;

private:
    /// A queued record paired with the scope of the tracer that produced it.
    ///
    /// `bytes` is the record's `EstimateRecordBytes` as measured on the way in.
    /// It is carried rather than recomputed on the way out so that the running
    /// total can never drift from what was added: the record is moved out of
    /// the queue before the subtraction would happen.
    struct QueuedSpan
    {
        internal::SpanRecord record;
        internal::InstrumentationScope scope;
        std::size_t bytes = 0;
    };

    struct WakeResult
    {
        std::vector<QueuedSpan> batch;
        bool done = false;
        std::size_t pending_flush_seq{0};
    };

    /// @brief Make the queue able to accept a record of @p record_bytes.
    ///
    /// Applies both caps — `max_queue_size` in records and
    /// `max_total_queue_bytes` in bytes — and counts one `QueueFull` per span
    /// actually lost, whether that is the incoming record (`DropNewest`) or an
    /// evicted one (`DropOldest`). Called with `m_mu` held.
    ///
    /// @param record_bytes the incoming record's `EstimateRecordBytes`.
    /// @return `true` when the caller may queue the record; `false` when the
    ///         incoming record is the one dropped.
    [[nodiscard]] bool MakeRoomFor(std::size_t record_bytes) noexcept;
    WakeResult WaitAndCollect() noexcept;
    [[nodiscard]] bool JoinWithTimeout(std::chrono::milliseconds timeout) noexcept;
    void WorkerLoop() noexcept;
    void ExportBatch(std::vector<QueuedSpan> batch) noexcept;
    /// @brief Count one dropped span against `reason`. No-op without a sink.
    ///        Lock-free, so it is safe to call under `m_mu`
    ///        (`docs/threading-model.md` §4).
    void RecordDropped(DropReason reason) noexcept;

    internal::IExporter* m_exporter;
    std::shared_ptr<const Resource> m_resource;
    /// Guarded by `m_mu` (ICP 0026). Every read already happened inside the
    /// lock — `OnEnd`/`MakeRoomFor` under the `scoped_lock`, `WaitAndCollect`
    /// under the `unique_lock` — and that is now normative rather than
    /// incidental: `SetOptions` writes it while the worker runs. No read may
    /// be hoisted into a local that outlives its critical section, and none
    /// into the worker's thread lambda.
    BatchOptions m_opts;
    std::uint32_t m_max_record_bytes;
    std::uint64_t m_max_total_queue_bytes;
    internal::IDiagnosticsSink* m_diag;

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<QueuedSpan> m_queue;
    /// Summed `QueuedSpan::bytes` of everything currently in `m_queue`.
    /// Guarded by `m_mu`.
    std::uint64_t m_queue_bytes{0};
    bool m_shutdown{false};
    std::size_t m_flush_seq{0};
    std::size_t m_flush_done_seq{0};
    std::condition_variable m_flush_cv;

    std::thread m_worker;
};

}  // namespace microtel::sdk
