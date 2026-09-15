# ICP 0026: the four hot-reload `Provider` setters

**Status:** Accepted — decided 2026-09-15. Docs only; the header and source
changes land in the v1.1 implementation packets named under Migration.
**Affected interfaces / docs:** [`include/microtel/provider.hpp`](../../include/microtel/provider.hpp)
(four new pure-virtual methods — **ABI change**),
[`include/microtel/status.hpp`](../../include/microtel/status.hpp)
(`Status` gains two enumerators), [`include/microtel/internal/sampler.hpp`](../../include/microtel/internal/sampler.hpp)
(`ISampler` gains one virtual with a default body),
[`docs/interfaces.md`](../interfaces.md) §4.5 (the `ISampler` Lifetime and
Threading paragraphs), [`docs/threading-model.md`](../threading-model.md) §4
(the lock table's `Held during` column), [`docs/configuration.md`](../configuration.md)
§3.11 (the #196 correction for `logging.level`). No CI, no runtime
dependency, no wire-format change.
**Affected tracks:** Track A — Trace/Metric/Log SDK (`src/sdk/`,
`src/common/`). Tracks B–F are untouched.

## Summary

Implement [ICP 0024](0024-v1.1-rescope.md)'s hot reload as four `noexcept`
`Provider` methods — `SetBatchOptions`, `SetMetricInterval`,
`SetSamplerRatio`, `SetLogLevel` — each validating its input, changing nothing
when it rejects, and returning the same `Status` the rest of the lifecycle
surface returns.

## Motivation

ICP 0024 took the setters over the control-plane socket and said, in its own
Migration section, that *"the four setters are a public-API addition and get
their own ICP when their signatures are drafted; this ICP schedules them, it
does not lock them."* This is that ICP.

**Adding methods to `Provider` needs one.** `Provider` is public API locked in
M0. [ICP 0023](0023-span-processor-scope.md) is the precedent that an addition
to a locked interface is ICP business even when no existing signature changes.
Here the additions are four pure virtuals on a class with a vtable, so v1.1
consumers **recompile**; `microtel-spec.md` §19 sanctions that in this window
(*"Binary ABI compatibility is best-effort within a minor release, **not**
guaranteed across minor releases"*), and [ICP 0025](0025-propagation-core.md)
already spends the same v1.0 → v1.1 ABI event on `TraceState`. Two additive
changes in one recompile, not two.

**Most of the work is already done, and that is the finding worth keeping.**
[`docs/control-plane-design.md`](../control-plane-design.md) §2 tiered the four
knobs by what they cost, and this ICP is a transcription of that tiering into
signatures. Tier 1 (batch options, metric interval) is free because every read
is already inside the owning object's mutex. Tier 2 (sampler ratio) is one
atomic. Tier 3 (log level) is free because the filter does not exist and
building it is unconstrained. Tier 4 (swapping the sampler *object*) is
deferred, for the reason recorded at the end of this document.

## Proposed change

### 1. Four methods on `Provider`

`include/microtel/provider.hpp`, appended after `GetLogger`. `BatchOptions`
lives in `sdk_builder.hpp`, which already includes `provider.hpp`, so it is
**forward-declared** here in the same block that already forward-declares
`Meter` and `Logger`; `LogLevel` comes from `microtel/log_sink.hpp`, which
includes nothing of ours.

```cpp
namespace microtel { struct BatchOptions; }   // defined in microtel/sdk_builder.hpp

/// @brief Retune the batching knobs of the span and log pipelines.
///
/// Mirrors `SdkBuilder::WithBatch` at runtime; one `BatchOptions` drives both
/// pipelines, exactly as at build time. `Unsupported` if the provider has no
/// batching span processor.
///
/// @threadsafety Thread-safe.
/// @noexcept
[[nodiscard]] virtual Status SetBatchOptions(const BatchOptions& opts) noexcept = 0;

/// @brief Retune the periodic metric reader's export interval.
///
/// Takes effect from the reader's next tick, not the current one. Applies to
/// a reader not yet built, so a later `GetMeter` uses the new value.
/// `Unsupported` if no metrics pipeline is configured.
///
/// @threadsafety Thread-safe.
/// @noexcept
[[nodiscard]] virtual Status SetMetricInterval(std::chrono::milliseconds interval) noexcept = 0;

/// @brief Retune the active sampler's sampling ratio.
///
/// `Unsupported` — and nothing changed — unless the configured sampler is a
/// `TraceIdRatio` sampler, or a composite that owns one. Never replaces a
/// sampler with one of a different kind.
///
/// @param ratio in `[0.0, 1.0]`. NaN and out-of-range values are rejected,
///        not clamped.
///
/// @threadsafety Thread-safe.
/// @noexcept
[[nodiscard]] virtual Status SetSamplerRatio(double ratio) noexcept = 0;

/// @brief Set the minimum severity for microtel's internal diagnostic logs.
///
/// Process-wide, not per-provider: the internal log path is a free function
/// (`microtel::internal::LogImpl`) with no provider in scope.
///
/// @threadsafety Thread-safe.
/// @noexcept
[[nodiscard]] virtual Status SetLogLevel(LogLevel level) noexcept = 0;
```

All four return `AlreadyShutDown` after `Shutdown`, and read `m_shut_down`
**before** taking any mutex, which is the pattern `GetMeter` and `GetLogger`
already use so a forked child cannot deadlock on a lock held by a thread that
no longer exists (`src/sdk/sdk_provider.cpp:GetMeter`, and
`docs/threading-model.md` §7).

### 2. `Status` gains `InvalidArgument` and `Unsupported`

`include/microtel/status.hpp`:

```cpp
    /// @brief The argument failed validation. Nothing was changed.
    /// Only returned by the `Set*` setters; never by `ForceFlush`/`Shutdown`.
    InvalidArgument = 4,

    /// @brief The knob does not exist on this provider — no metrics pipeline,
    /// or a sampler with no ratio. Nothing was changed, and this is not an
    /// error condition of the pipeline. Setters only.
    Unsupported = 5,
```

`Status` is the right return type rather than `Expected<void, ConfigError>`:
hard rule 17's four-way value already carries `AlreadyShutDown`, which every
setter needs and which `ConfigError` has no room for; and `Status` keeps the
setters allocation-free and `noexcept`, where building a `ConfigError`
allocates a `std::string`. What `Status` lacks is *why*, so two enumerators are
added — the operator distinction that matters is "your value is wrong" versus
"this provider has no such knob", and `Failed` ("an unrecoverable internal
error occurred") is the wrong word for either. The precise detail — which
field, what range, which sampler — goes to the internal log at `Warn`, which
is also what makes `SetLogLevel` self-consistent: a rejected setter call is an
internal log like any other.

`Status`'s own Doxygen ("Outcome of a lifecycle operation (`ForceFlush`,
`Shutdown`)") is amended to name the setters. `ForceFlush` and `Shutdown`
still return only the original four.

### 3. Validation

| Setter | Rejected with `InvalidArgument` |
|---|---|
| `SetBatchOptions` | `max_queue_size == 0`; `max_export_batch_size == 0`; `max_export_batch_size > max_queue_size`; `schedule_delay <= 0ms` |
| `SetMetricInterval` | `interval <= 0ms` |
| `SetSamplerRatio` | NaN; `ratio < 0.0`; `ratio > 1.0` |
| `SetLogLevel` | a value outside the declared `LogLevel` enumerators |

The zero and non-positive rules are not arbitrary: a zero
`max_export_batch_size` makes `WaitAndCollect`'s drain count zero forever, and
a zero `schedule_delay` or `interval` turns the worker's `wait_for` into a
spin. **The setters are stricter than the builder** — see Discrepancies.

### 4. Where each setter writes, and under which lock

**`SetBatchOptions`.** `BatchSpanProcessor` and `BatchLogRecordProcessor` each
gain a concrete `void SetOptions(const BatchOptions&) noexcept` that takes
`m_mu`, assigns `m_opts`, and `m_cv.notify_one()`s. Neither
`internal::ISpanProcessor` nor `internal::ILogRecordProcessor` gains a method:
batching knobs belong to the processors that batch, not to the processor
contract. `SdkProvider` reaches the concrete types through two borrowed,
non-owning pointers set where each owning `unique_ptr` is assigned —
`SdkBuilder::Build` for the span processor (`BuildSpanProcessor` already
returns `std::unique_ptr<sdk::BatchSpanProcessor>`, and the concrete type is
erased only at the `Deps` boundary) and `SdkProvider::GetLogger` for the log
processor. Null means the pipeline does not exist.

The call is **two phases, never nested**: retune the span processor; then,
under `m_logger_mu`, store `m_log_batch_opts` (the seed a later `GetLogger`
builds from) and read the borrowed log-processor pointer; release
`m_logger_mu`; then retune the log processor. That is the shape
`SdkProvider::LogProcessorPtr` exists for — its Doxygen defers to
`MetricReaderPtr`'s, which gives the reason: *"taking `m_meter_mu` and then the
reader's own lock would nest two non-leaf locks, which
`docs/threading-model.md` §4 forbids."* The consequence,
stated rather than hidden: **the change is not atomic across the two
pipelines** — for the duration of one call a span batch may be cut under the
new options while a log batch is still under the old.

**`SetMetricInterval`.** `PeriodicExportingMetricReader` gains
`void SetInterval(std::chrono::milliseconds) noexcept` — take `m_mu`, assign
`m_interval`, release. `SdkProvider` writes `m_metric_interval` under
`m_meter_mu` and reads the borrowed reader pointer there
(`SdkProvider::MetricReaderPtr`), then calls `SetInterval` after releasing.
**`m_wake` is deliberately not set**: waking the reader would force an
immediate collect+export cycle, which changing an interval should not do.

**`SetSamplerRatio`.** No provider lock at all. `m_sampler` is a
`SamplerHandle` assigned once at construction and never reassigned, so the
raw `internal::ISampler* m_sampler` that every `SdkTracer` caches
(`src/sdk/sdk_tracer.hpp:m_sampler`) keeps pointing at the same live object.
That is the whole reason the ratio-only design is safe and the object swap is
not. See §5 and "Deferred".

**`SetLogLevel`.** A `std::atomic<LogLevel>` in `src/common/log_sink.cpp`
beside the existing `SinkState`, read by `LogImpl` with one relaxed load
before it touches the sink mutex. See §6.

### 5. `SetSamplerRatio` fails loudly, and never swaps sampler kind

`internal::ISampler` gains one virtual **with a default body**, so no existing
implementation, mock, or fake changes:

```cpp
    /// @brief Retune this sampler's ratio in place, if it has one (ICP 0026).
    ///
    /// Default: `false` — a sampler with no ratio is never converted into one
    /// that has a ratio. A composite forwards to the delegate it owns and,
    /// on success, regenerates its own `Description`.
    ///
    /// @param ratio already validated to be in `[0.0, 1.0]` and not NaN.
    /// @return `true` if this sampler or a delegate it owns applied the ratio.
    ///
    /// @threadsafety Thread-safe. Must not invalidate a `Description()` view
    ///               previously returned to a concurrent caller.
    [[nodiscard]] virtual bool TrySetRatio(double ratio) noexcept { return false; }
```

`AlwaysOnSampler` and `AlwaysOffSampler` inherit the default and
`SetSamplerRatio` returns `Unsupported`. `ParentBasedSampler` forwards to
`m_root`, because `parentbased_traceidratio` is the deployment shape an
operator most wants to retune. A `false` from the root is a `false` from the
chain: nothing is changed anywhere and the caller is told.

**The hot path stays one atomic load.** `TraceIdRatioSampler` today holds
`double m_ratio`, `bool m_always_sample`, `std::uint64_t m_threshold`, and a
`std::string m_description`, and `ShouldSample` reads the middle two
(`src/sdk/sampler_factories.cpp:SampleDecision`). Two separate atomics could be
read torn, so `m_always_sample` is **folded into the threshold**:
`ComputeThreshold` already returns `UINT64_MAX` if and only if `ratio >= 1.0`
(for any `ratio` below 1.0 the largest product is `2^64 - 2^11`, so the value
is representable and strictly smaller), and the decision becomes one
`std::atomic<std::uint64_t>` relaxed load plus the sentinel test. Decisions are
bit-for-bit what they are today; `memory-model.md` §8.1 and §4.5's LOCKED
no-allocation-on-the-hot-path rule hold, because nothing allocates and nothing
locks.

**Descriptions are regenerated, and old views stay valid.** `Description()`
returns a borrowed `std::string_view`, so the description string may not be
mutated or freed under a concurrent reader. `TraceIdRatioSampler` therefore
keeps an append-only `std::deque<std::string> m_descriptions` (stable element
references) guarded by a leaf `m_desc_mu` taken only by `TrySetRatio`, plus a
`std::atomic<const std::string*>` the reader loads. A successful retune appends
the new rendering and publishes it; every previously handed-out view stays
valid for the sampler's life. A retune to the ratio already in force appends
nothing. The bound on growth is one short string per *accepted* call, which is
operator cadence, not a hot path.

**Sampler chains inherit the obligation.** v1.1's `microtel::sampler::Chain`
(roadmap §4 v1.1, packet 2.1) composes descriptions the way
`ParentBasedSampler` already does — its `m_description` is built at
construction from its children's, so a chain wrapping a ratio sampler embeds
the ratio and goes stale the moment the ratio moves. Every composite that
forwards `TrySetRatio` successfully **regenerates its own description the same
way, under its own setter lock**, before returning `true`. That is stated here
so packet 2.1 inherits it rather than rediscovering it.

`docs/interfaces.md` §4.5's Lifetime paragraph — *"Replaceable in v1.1's
hot-reload path; in v1, immutable after `Build`"* — is now wrong in both
halves and is amended: the sampler object is **not** replaceable in v1.1; its
ratio is retunable in place, and the object identity is fixed for the
provider's life. §4.5's Threading paragraph gains the `Description()` view
lifetime. Neither sentence is LOCKED; §4.5's two LOCKED markers (`ShouldSample`
thread-safety, and no allocation on the hot path) are untouched and stay true.

### 6. The log-level filter (resolves #190)

**There is no log-level filtering anywhere today.** `LogImpl`
(`src/common/log_sink.cpp:LogImpl`) performs no level check — the `level`
parameter only selects a string tag — and `config::Config` has no logging
member. `docs/configuration.md` §3.11 records the same thing from the other
side: the `logging.level` key and `MICROTEL_LOG_LEVEL` are *"read by
nothing"* (correction #196). So packet 2.4 **builds** the filter; it does not
expose an existing one.

- `std::atomic<LogLevel> g_min_level{LogLevel::Info}` in
  `src/common/log_sink.cpp`, beside `SinkState`. `LogImpl` returns immediately
  when `level < g_min_level`, before it takes the sink mutex. An atomic is
  race-free by construction; no lock is added to the internal log path.
- Seeded at `Build()` from a new `logging.level` TOML key and
  `MICROTEL_LOG_LEVEL`, under the existing precedence rules
  (`docs/configuration.md` §1). The default `Info` changes nothing observable:
  all three production `LogImpl` call sites emit at `Warn` (two in
  `SdkBuilder::Build`'s validation warnings, one in `GrpcWireCodec`).
- `Provider::SetLogLevel` is a thin forwarder to an internal setter, so the
  operator surface is uniform across the four knobs. Because the knob is
  process-global, **v1.1's multi-profile providers share it**: last writer
  wins. Stated, not worked around — a per-provider internal log level would
  need a provider in scope at every emission site, which `LogImpl` does not
  have and is not worth growing one for.

**The #190 decision is option (1): the `LogSink` hook only; spdlog stays
consumer-side.** Issue #190 asks what happens to
`libmicrotel_common.a`'s link closure when the internal log route finally does
something, and lists three options. Option (1) is taken:

- `LogImpl` keeps its stderr fallback and **never calls spdlog**, now or
  later. `libmicrotel_common.a` therefore never acquires an undefined spdlog
  reference, which is the defect #190 exists to prevent.
- The `$<BUILD_INTERFACE:>` wrapping in the install rules and the absent
  `find_dependency(spdlog)` in `cmake/microtelConfig.cmake.in` stay correct as
  written, and the F2 consumer smoke test (ICP 0020 Decision 6) keeps passing
  against a `MICROTEL_USE_SPDLOG=ON` install tree — by construction rather
  than by a new test.
- Applications that want spdlog install `microtel_spdlog_bridge`
  (`src/adapters/spdlog/CMakeLists.txt`, an `INTERFACE` target that links
  `spdlog::spdlog` inside the *consumer's* build) as their `LogSink`. That
  keeps rule 12's "optional spdlog" genuinely optional.
- Consequently the *"M3 wires the spdlog route"* comments in
  `src/common/log_sink.cpp` and `src/common/internal_log.hpp` are retired by
  packet 2.4: there is no spdlog route inside microtel. Whether
  `MICROTEL_USE_SPDLOG` survives as an option is packet 2.4's call — it
  currently gates a `PRIVATE` compile definition and a link that contributes
  zero symbols.

**#190 is resolved by this decision and closed by packet 2.4**, which is the
commit that makes the claim checkable.

## The widened invariants, precisely

Every one of these is a documentation and writer change, not a structural one:
the reads are already under the lock. What widens is what the lock is
*declared* to guard, and the rule that nothing may cache around it.

1. **`BatchSpanProcessor::m_mu` guards `m_opts`.** Today it is declared to
   guard the queue, `m_queue_bytes`, `m_shutdown` and the flush sequence
   numbers, and it happens to cover every `m_opts` read —
   `OnEnd`/`MakeRoomFor` under the `scoped_lock`, and `WaitAndCollect` under
   the `unique_lock`. That becomes normative. `m_opts` is never captured into
   a local that outlives the critical section, and never into the worker's
   thread lambda.
2. **`BatchLogRecordProcessor::m_mu` guards `m_opts`**, identically — same
   reads in `OnEmit` and `WaitAndCollect`, same rule.
3. **`m_max_record_bytes` and `m_max_total_queue_bytes` are not retunable.**
   They come from `MemoryLimitOptions`, not `BatchOptions`, and stay immutable
   after construction. `SetBatchOptions` does not touch them.
4. **`PeriodicExportingMetricReader::m_mu` guards `m_wake` *and*
   `m_interval`.** Its header comment says `// guards m_wake` today; the read
   at `RunLoop` is already inside `unique_lock{m_mu}`, so only the comment and
   the writer are new. `m_temporality`, `m_producer` and `m_exporter` stay
   immutable after construction.
5. **A worker may cache a knob for the length of one wake, and no longer.**
   Reading `max_export_batch_size` twice inside one hold of `m_mu` is fine;
   hoisting it into a local that survives the release is not.
6. **A shortened delay takes effect from the next wait.**
   `std::condition_variable::wait_for(lock, d, pred)` fixes its deadline from
   `d` at entry, and a notify whose predicate is still false does not shorten
   it. So a shortened `schedule_delay` or `interval` costs at most one
   old-length tick before it applies. The `notify_one` in
   `BatchSpanProcessor::SetOptions` is still worth doing for the other half of
   the change: it makes the worker re-evaluate its predicate against the
   **new** `max_export_batch_size`, which can drain immediately if the queue
   already exceeds it.

**No LOCKED sentence in `docs/threading-model.md` has to be amended.** This
was checked marker by marker. §4's four LOCKED rules survive as written: the
setters take exactly one non-leaf lock each, assign, notify, and release, so
rule 2 (*"A thread holds at most one non-leaf lock at a time"*) holds — which
is precisely why `SetBatchOptions` is specified as two phases in §4 above
rather than one nested one. §3.1's LOCKED backpressure and
producer-never-waits rules are about `OnEnd`, which is unchanged. §10's
`Provider` row stays "Thread-safe". What changes is the **`Held during`**
column of §4's lock table — three rows, none of them LOCKED — plus the
`ISampler` paragraphs in `interfaces.md` §4.5 named in §5 above.

## Discrepancies found while writing this

Recorded rather than silently designed around, per the repository's own rule
about documents that assert the opposite of the code.

1. **The builder validates one batch rule; the setters validate four.**
   `config::Validate` (`src/common/config/config_validator.cpp:Validate`)
   rejects only `max_export_batch_size > max_queue_size`. A zero
   `max_queue_size`, a zero `max_export_batch_size` or a zero `schedule_delay`
   passes `Build()` today and produces a processor that never drains or spins.
   The setters reject all three. Tightening `Build()` to match is a
   one-function change that belongs to packet 2.4 or a follow-up issue; it is
   **not** folded into this ICP, because it changes what an existing
   configuration does at startup.
2. **`MakeTraceIdRatioSampler(NaN)` is undefined behaviour today.**
   `std::clamp(NaN, 0.0, 1.0)` returns NaN (neither comparison is true),
   `m_always_sample` is then false, and `ComputeThreshold` falls through both
   guards to `static_cast<std::uint64_t>(NaN * max_d)` — a cast of a
   non-representable value. Pre-existing, unrelated to hot reload, and found
   only because `SetSamplerRatio` had to decide what to do with NaN. The
   setter rejects it; packet 2.2 fixes the factory in the same file.
3. **The factory clamps, the setter rejects.** `MakeTraceIdRatioSampler`
   documents clamping to `[0.0, 1.0]` (`include/microtel/sampler.hpp`) and
   that stays. The setter rejects out-of-range values instead, deliberately:
   at build time a clamp is a documented convenience, but at reload time a
   caller asking for `1.5` has a bug in their administrative surface, and
   silently clamping hides it from the operator watching the return value.
4. **`SimpleSpanProcessor` is not reachable from `SdkBuilder`.**
   `BuildSpanProcessor` always constructs a `BatchSpanProcessor`; nothing in
   `src/` constructs a `SimpleSpanProcessor`. So `SetBatchOptions`'s
   `Unsupported` return is, today, unreachable in a builder-constructed
   provider. It is specified anyway, because the owning member is typed
   `std::unique_ptr<internal::ISpanProcessor>` and a setter that assumes its
   dynamic type would be a latent trap.

## Migration

Nothing to do today; this ICP schedules work. Per the
[ICP 0025](0025-propagation-core.md) precedent, the normative documents are
**not** amended by this PR — writing a claim about `SetBatchOptions` into
`threading-model.md` before the method exists is the failure ICP 0021 exists to
stop. The amendments are obligations of the packets:

- **Packet 2.2 — the setters.** `provider.hpp`, `status.hpp`,
  `internal/sampler.hpp`, `sampler_factories.cpp`,
  `batch_span_processor.{hpp,cpp}`, `batch_log_record_processor.{hpp,cpp}`,
  `periodic_exporting_metric_reader.{hpp,cpp}`, `sdk_provider.{hpp,cpp}`,
  `sdk_builder.cpp`, and `tests/fakes/fake_provider.hpp`. Amends
  `docs/threading-model.md` §4's lock table and `docs/interfaces.md` §4.5.
  Adds the TSAN hammer test and the validation fuzz harness that v1.1's
  ships-when gate clause 2 requires ([ICP 0024](0024-v1.1-rescope.md)).
- **Packet 2.4 — the log-level filter (#190).** `log_sink.cpp`,
  `internal_log.hpp`, the `logging.level` config key and `MICROTEL_LOG_LEVEL`,
  and the `SetLogLevel` forwarder. Retires §3.11's #196 correction for
  `logging.level` only — `logging.sink` and `logging.file` remain
  unimplemented, being the sink's business rather than the level's. Closes
  #190 with the option-(1) decision recorded above.

**Consumers:** recompile against v1.1 headers; no source change is required
unless they implement `Provider` themselves, which in-tree only
`tests/fakes/fake_provider.hpp` does. One in-tree switch is exhaustive over
`Status` without a `default` — `StatusToString` in `tests/consumer/main.cpp`,
the F2 smoke test — and gains two cases under `-Wswitch`; external code with
the same shape sees the same warning.

## Deferred: swapping the sampler *object*

Replacing a provider's sampler with one of a **different kind** stays out of
v1.1, as `docs/control-plane-design.md` §2's Tier 4 already decided and
[ICP 0025](0025-propagation-core.md) restated. The reason is a borrowed
pointer, not a missing feature: `SdkTracer` caches
`internal::ISampler* m_sampler` handed over at `GetTracer` time and
dereferences it on the hot path with no synchronisation. A live swap is a data
race on that pointer *and* a use-after-free on the pointee for every
`SdkTracer` the application still holds. Fixing it needs an atomic pointer plus
deferred reclamation, or an `atomic<shared_ptr>` load that puts a refcount
round-trip inside `StartSpan` — the `noexcept` hot path the project's
performance claims rest on. `SetSamplerRatio` avoids all of it by never
changing the object's identity, which is the entire design.

If the swap is ever wanted it gets its own ICP, pricing the hot-path cost
against the M7 benchmark numbers.

## Rationale & alternatives

- **`Expected<void, ConfigError>` instead of `Status`** — rejected. It carries
  a field path and a message, which reads better, but it has no
  `AlreadyShutDown`, it allocates a `std::string` on the failure path inside
  what should be a `noexcept` method, and it makes the setters the only
  lifecycle-shaped methods on `Provider` that do not return `Status`. Two
  enumerators plus an internal log at `Warn` buys the same operator
  information at none of that cost.
- **Reuse `Status::Failed` for both rejection cases** — rejected. Its Doxygen
  is *"an unrecoverable internal error occurred"*; a ratio of `1.5` and a
  provider with no metrics pipeline are neither unrecoverable nor internal,
  and collapsing them hides exactly the distinction ICP 0024's gate clause 2
  asks a fuzz harness to exercise.
- **A `Signal` selector on `SetBatchOptions`** — rejected as speculative.
  `SdkBuilder` has one `WithBatch` feeding both pipelines
  (`sdk_builder.cpp` assigns `.log_batch_opts = cfg.batch`); a runtime setter
  that splits what build time unifies would be a new configuration axis
  arriving through the hot-reload door. If per-signal batching is wanted it
  belongs to `SdkBuilder` first.
- **Put `SetBatchOptions` on `ISpanProcessor`** — rejected. It would force
  `SimpleSpanProcessor`, both mocks and both fakes to answer a question about
  batching that they have no business answering, and ICP 0023 has just
  finished paying for one signature change on that interface.
- **Clamp instead of reject** — rejected; see Discrepancy 3.
- **Wake the metric reader on an interval change** — rejected. It conflates
  "export at a different cadence" with "export now", and `ForceFlush` already
  means the second.
- **Leave `Description()` reporting the configured ratio** — rejected. An
  operator who retunes to 0.01 and reads back
  `TraceIdRatioSampler{0.100}` has been told something false by a diagnostic
  surface, which is worse than the append-only deque costing one short string
  per accepted call.
- **Per-provider internal log level** — rejected; see §6. `LogImpl` is a free
  function called from code that has no provider in scope.
