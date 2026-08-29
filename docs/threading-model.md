# microtel Threading Model

**Status:** M0 deliverable. Normative for which threads exist, what they own, and how data crosses them in v1.
**Companion documents:** `architecture.md` (layered structure), `memory-model.md` (resource ownership), `error-model.md` (no-exceptions-across-threads rule), `interfaces.md` (per-method threading tags).
**Source of truth for rationale:** `microtel-spec.md` §5.1, §5.3.

---

## 1. Purpose and authority

This document is the canonical answer to:

1. **Which threads exist in microtel and what does each own?**
2. **For every public method, which thread is the caller expected to be on?**
3. **For every cross-thread hand-off, what's the synchronisation contract?**
4. **What does it mean concretely that the caller-thread API is `noexcept` and non-blocking?**

Doxygen `@threadsafety` tags on individual methods reference categories defined here.

Some rules are non-negotiable in v1; they are flagged **(LOCKED)**. Changing a (LOCKED) rule requires an ICP.

---

## 2. The three threads

v1 has exactly three thread roles. There is no thread pool, no fiber scheduler, no work-stealing in v1.

### 2.1 Caller thread (any application thread)

**Identity.** Any thread that calls a public microtel API. Plural — many caller threads share the role. The thread is owned by the application; microtel never creates a caller thread.

**Owns:** the `Span` handles handed back from `StartSpan`. RAII-auto-end fires on the caller thread when the handle goes out of scope.

**May call:** the public API in `include/microtel/`. Specifically `Tracer::StartSpan`, `Span::SetAttribute / AddEvent / AddLink / SetStatus / End`, the propagator inject/extract methods, `Provider::ForceFlush`, `Provider::Shutdown`, `Provider::GetExporterHealth`.

**May not:** access the SDK's internal queues, the encoder, the wire codec, the transport, or any internal interface directly. Caller-thread code never includes a header from `include/microtel/internal/`.

**Hot-path guarantee (LOCKED).** `StartSpan`, `SetAttribute`, `AddEvent`, `AddLink`, `End` are `noexcept` and never wait on I/O. See §8 for the precise contract.

### 2.2 Exporter worker thread (one per `Provider`)

**Identity.** Owned by the `BatchSpanProcessor`. Created at `Provider` construction; joined at `Provider::Shutdown` or destruction.

**v1 always has exactly one worker per process** (LOCKED). Multi-profile (multiple `Provider` instances per process) is a v1.1 feature; v1 supports a single global `Provider` only.

**Owns:** batch construction state, retry orchestration state for the in-flight batch, the per-thread randomness source for backoff jitter.

**Drains:** the span queue (consumer side; multiple producers).
**Calls into:** `IOtlpEncoder::Encode` (synchronously), `IWireCodec::Send` (synchronously, but the I/O it triggers happens on the I/O thread), `IDiagnosticsSink::Record*`.

**May not:** call any caller-facing API. The worker thread never invokes `Tracer::StartSpan` or any other public API; doing so would risk a queue self-feed.

**Sleep state.** The worker waits on a condition variable (or eventfd) when the queue is empty and no batch deadline is pending. Wake sources: enqueue notification, batch-deadline timer, `ForceFlush` request, `Shutdown` request.

### 2.3 I/O thread (one per process)

**Identity.** Owned by the `Transport`. Created at `Transport::Connect`; joined at `Transport::Close`.

**v1 always has exactly one I/O thread per process** (LOCKED). One nghttp2 session, one socket, one reactor.

**Owns:** the OpenSSL `SslCtx` reference, the `SslSession`, the `Nghttp2Session`, the `Socket`, the `IReactor` (epoll on Linux, kqueue on BSD/macOS), the per-stream in-flight request state.

**Calls into:** OpenSSL, nghttp2, libc syscalls. Receives completion notifications from nghttp2 callbacks.

**May not:** call into the encoder, the SDK, or any caller-facing API. The I/O thread is purely the byte-mover.

**Sleep state.** Blocked in `epoll_wait` / `kevent` with a timeout that is the nearest of: pending HTTP/2 timeout, retry-after deadline, shutdown deadline. Wake sources: socket-readable / -writable, eventfd from the request queue, eventfd from the shutdown signal.

---

## 3. Inter-thread channels

Three channels, in canonical order. Each has a fixed shape, owner, and synchronisation contract.

### 3.1 Caller → exporter worker — the span queue

**Producer:** any caller thread, on `End()`.
**Consumer:** the exporter worker, in batches.

**Shape.** Bounded MPSC queue. Capacity is `max_queue_size` from the batch processor configuration (default 8192 records; spec §6.1).

**Backpressure (LOCKED).** When the queue is full, the producer **drops the incoming record** by default (`drop_newest`). The producer never blocks. The drop is recorded against the `queue_full` counter (`error-model.md` §3). Drop-oldest is an opt-in alternative (spec §5.4); when configured, the worker thread (not the producer) is responsible for shedding the oldest entry on overflow.

**Producer-side synchronisation contract.** The enqueue path:

1. Acquires a slot in the bounded queue using a wait-free or near-wait-free protocol.
2. Moves the `Span` record into the slot.
3. Publishes the slot.
4. If the queue transitioned from empty to non-empty, signals the worker's wakeup primitive.

The exact data-structure choice (lock-free atomic ring vs. mutex-protected ring vs. linked list with per-thread freelists) is a v1 implementation decision pinned during M3 against benchmark evidence. The architectural contract M0 commits to is the four numbered guarantees above plus:

- **Producer never waits on I/O** (LOCKED).
- **Producer never holds a lock spanning the move-into-slot step** if a mutex implementation is chosen — the lock window is bounded to slot acquisition, not the move payload work.
- **Allocation in the producer path is bounded to `O(1)` and may be zero** depending on implementation; see `memory-model.md` §8.2.

**Consumer-side semantics.** The worker drains up to `max_export_batch_size` records (default 512; spec §6.1) per batch cycle. Drain is non-blocking with respect to producers — the worker never blocks producers, even briefly. After drain the worker releases the slots back for re-use.

**Wakeup primitive.** A `std::condition_variable` paired with the queue's mutex if the implementation uses one, or a `eventfd(2)` on Linux / pipe-pair on BSD that the worker waits on alongside its deadline timer. Implementation choice is M3-era; the contract is "the worker can sleep until either an enqueue or a deadline fires."

### 3.2 Exporter worker → I/O thread — the transport request queue

**Producer:** the exporter worker, when it calls `IWireCodec::Send` and the codec hands a request to the transport.
**Consumer:** the I/O thread, in its reactor loop.

**Shape.** SPSC (single-producer, single-consumer) bounded queue. Capacity is small — the spec allows at most a few outstanding batches (`microtel-spec.md` §5.2 — one HTTP/2 connection per endpoint, multiplexed; outstanding batches are bounded by the codec's own configuration).

**Producer-side contract.** The exporter worker calls into the transport synchronously; the transport acquires the request-queue lock, pushes a request descriptor (carrying a borrowed `std::span<const std::byte>` over the `EncodedPayload` bytes — see `memory-model.md` §3.3), wakes the I/O thread via eventfd, releases the lock, returns to the worker. The worker then awaits a completion (described in §3.3 below).

**Consumer-side contract.** The I/O thread's reactor wakes on the eventfd, drains pending requests under the same lock, attaches each to a new nghttp2 stream, and returns to its reactor sleep until socket activity or another wake.

**Backpressure.** If the request queue is full (very rare in v1 because outstanding batches are tightly bounded), the transport returns a `WireCodec`-visible failure with reason `transport_busy`. The codec surfaces this as a `WireResult` with `retryable=true` and a small backoff. The I/O thread does not exert backpressure on the worker beyond this.

### 3.3 I/O thread → exporter worker — request completion

**Producer:** the I/O thread, when nghttp2 emits the response (HEADERS + DATA + trailer HEADERS, or trailer-only HEADERS).
**Consumer:** the exporter worker, which is parked waiting on the completion.

**Shape.** A per-request completion future. The exporter worker, after handing a request to the transport (§3.2), waits on a `std::condition_variable` keyed to the in-flight request. The I/O thread, on completion, copies the response bytes from nghttp2's owned buffers into the codec's response buffer (sized at `max_response_bytes` from `memory-model.md` §6), populates the completion record, and signals the condvar.

**Why copy at the boundary.** nghttp2 owns its receive buffers and may recycle them on subsequent reads. Copying the bytes into a codec-owned buffer (bounded by `max_response_bytes`) means the worker can parse the response without holding a reference into nghttp2 internals. Copy cost is well within the budget — responses are tiny relative to the request and the parse work.

**On the worker side.** The worker calls into `IWireCodec::Send`, which internally enqueues to the transport (§3.2) and then waits on the completion. When the wait returns, the codec parses the copied response bytes into a `WireResult` and returns up through the exporter. The worker's `Send` call appears synchronous to the worker — but its **internal** behaviour spans both the worker thread (enqueue, parse) and the I/O thread (write, read).

**Cancellation.** If the exporter aborts the request (timeout, `Shutdown` mid-flight), it signals the I/O thread to send `RST_STREAM` (gRPC) or close the stream (HTTP); the I/O thread completes the request record with a cancellation result; the worker observes the result and returns. The completion path is symmetric whether the result is success, failure, or cancellation.

---

## 4. Lock-ordering rules

Locks in v1, from leaf to root in the partial order:

| Lock | Owner | Held during |
|---|---|---|
| `m_diag` | `IDiagnosticsSink` | counter increment, ring-buffer write |
| `m_queue` | `BatchSpanProcessor` | enqueue / dequeue (if a mutex implementation is chosen) |
| `m_transport_request` | `Transport` | request-queue push / drain |
| `m_completion` | per-request | completion record fill / wait |
| `m_shutdown` | `Provider` and `Transport` | shutdown state machine transitions |

**Rules (LOCKED).**

1. **`m_diag` is a leaf** — no lock from this table is acquired while `m_diag` is held. Diagnostic counters are designed so the increment path is short and self-contained. Where a counter increment can be done with `std::atomic<uint64_t>::fetch_add`, no lock is taken at all.
2. **A thread holds at most one of `{m_queue, m_transport_request}` at a time.** The queue lock is dropped before the transport lock is acquired, and vice versa.
3. **`m_completion` is acquired *after* `m_transport_request`** when the transport pushes a new request, *or* without `m_transport_request` when the I/O thread completes a request (it locates the completion record by request ID, which is itself stored under `m_transport_request`, but releases that lock before acquiring the per-request `m_completion`).
4. **`m_shutdown` is acquired only at state transitions** (start of `Shutdown`, observation of shutdown by worker / I/O thread). It is never held while `m_queue`, `m_transport_request`, or `m_completion` is held.

The rules collapse to a simple practical statement: **at most one non-leaf lock is held at any time**. v1's design intentionally avoids nested locks. Any future code that wants to break this needs an ICP.

---

## 5. Wakeups and shutdown signalling

Two wakeup mechanisms, used uniformly:

### 5.1 In-process wakeups for waiting threads

- **Exporter worker:** waits on `std::condition_variable_any` paired with the queue's wakeup state. The worker's wait predicate is `(queue_non_empty || deadline_reached || shutdown_requested)`.
- **I/O thread:** waits in `epoll_wait` / `kevent`. Two file descriptors are registered besides the socket: `m_request_eventfd` (woken when the transport request queue gets a new entry) and `m_shutdown_eventfd` (woken when shutdown is requested).

### 5.2 Cross-thread wakeups for completions

The I/O thread signals a per-request `m_completion` condvar when a request completes. The waiting worker is parked on that condvar.

### 5.3 Shutdown signal (LOCKED — single-source-of-truth)

A single `std::atomic<ShutdownState> m_state` on the `Provider` is the ground truth for shutdown progress:

```
NotShutDown → Draining → Closed
```

- `Provider::Shutdown(timeout)` is the only writer to the state. It acquires `m_shutdown`, transitions `NotShutDown` → `Draining`, then signals both wakeup primitives (worker condvar, I/O eventfd).
- The worker, on observing `Draining`, finishes its in-flight batch within the remaining timeout, drains the queue up to the deadline, then exits its loop. `Span` records that arrive after the worker has exited are dropped at the producer side with reason `post_shutdown`.
- The I/O thread, on observing `Draining`, completes any in-flight request, runs the GOAWAY-and-drain handshake on the nghttp2 session, closes the socket, exits its loop.
- After both threads have joined, `Shutdown` transitions `Draining` → `Closed` and returns the appropriate `Status` (see `error-model.md`).

`Shutdown` is **idempotent** (LOCKED). A second call observes `Closed` and returns `AlreadyShutDown` immediately.

---

## 6. `ForceFlush` and `Shutdown` lifecycle

### 6.1 `ForceFlush(timeout)`

**Caller.** Any caller thread, including from the application's main thread before `Shutdown`.
**Effect.** Wakes the worker with a flush request; the worker drains the queue (without exiting its loop) and waits for the in-flight batch to complete or for the timeout to elapse. Returns one of `Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`.

The worker treats a `ForceFlush` request as a synthesised batch deadline of "now". After the queue is empty and the in-flight request returns, the worker signals the flush-completion future and returns to its normal loop.

`ForceFlush` does not stop accepting new records. New `End()` calls during a flush proceed normally; if they enqueue after the flush's drain pass, they will be picked up on the next batch cycle, not the flush.

### 6.2 `Shutdown(timeout)`

**Caller.** Any caller thread, but in practice the application's shutdown path.
**Effect.** Transitions `m_state` to `Draining`, signals the worker and I/O thread, waits for both to join (within the timeout), transitions to `Closed`. Returns `Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`.

After `Shutdown` returns, no further records are accepted; producers see `post_shutdown` drops.

**Destructor of `Provider`.** Invokes `Shutdown(small_finite_timeout)` if not already shut down. The destructor itself is `noexcept` (LOCKED) — if `Shutdown` returns `TimedOut` or `Failed`, the destructor logs a diagnostic and returns. It does not block indefinitely.

The full sequence diagram for `Shutdown` is `docs/sequences/shutdown-drain.md`.

---

## 7. Fork semantics

Forking a process that has microtel running raises real correctness questions because the child inherits half-finished state (mid-flight nghttp2 stream, half-written socket buffers, locked mutexes that the worker thread no longer exists to release).

**Rule (LOCKED).** After `fork()`, the child process starts with **exporter workers disabled** until the application explicitly reinitialises microtel.

Concretely:

- A `pthread_atfork` handler runs in the child and marks the live `Provider`
  shut down. Worker and I/O threads are not present in the child (only the
  forking thread survives `fork`), so any API entry point that consults the
  shutdown flag before touching shared state drops instead of blocking.

  The mechanism is a flag, not a state machine: this section previously
  specified `m_state = Closed`, and no such member has ever existed
  (see ICP 0018 and issue #134). The flag it sets is
  `SdkProvider::m_shut_down`, and the child handler does nothing but one
  atomic store — it takes no lock, because a lock held at `fork()` time by a
  thread that does not exist in the child would never be released.

  **This does not make a forked child fully safe, and the previous wording
  overstated it.** It covers entry points that check the flag first —
  `GetLogger` and `GetMeter`. It does not cover `Span::End()`, which reaches
  `BatchSpanProcessor::OnEnd` and takes that processor's mutex with no path to
  the flag; a child calling it can still deadlock on a mutex held at fork time.
  Closing that needs a per-component fork check, tracked separately.
- There is **no parent and no prepare handler.** The rule previously asked the
  parent handler to "record a diagnostic that fork was observed"; there is
  nothing to record it to. `LogImpl` has no production call sites and is not
  async-signal-safe, and no `DropReason` covers fork. Registering an empty
  handler would only disguise that.
- `SdkBuilder::Build()` may be called again in the child to construct a fresh `Provider`. The new provider initialises fresh threads, sockets, and nghttp2 sessions. The parent's I/O state is **not shared**.

The fork-survival sequence diagram is `docs/sequences/fork-survival.md`.

---

## 8. What `noexcept` and non-blocking on the hot path mean

This section is the precise contract the API delivers to caller threads. The numbered guarantees are (LOCKED).

For every method on `Tracer` and `Span` listed in §2.1:

1. **`noexcept`**: the method is declared `noexcept` and will not throw. Implementation is responsible for catching anything that would unwind and converting it into a drop-and-count.
2. **No I/O**: no syscall that can block on network, disk, or DNS. `clock_gettime(CLOCK_MONOTONIC)` is permitted and treated as effectively non-blocking.
3. **No allocation on the unsampled path**: see `memory-model.md` §8.1.
4. **Bounded allocation on the sampled path**: bounded by the span structural limits in `memory-model.md` §7.
5. **Bounded synchronisation**: no mutex held for unbounded duration. Lock windows are `O(1)` work, not `O(batch size)`.
6. **No exceptions across thread boundaries**: an internal failure on the caller thread (e.g., a queue-overflow drop) is recorded as a diagnostic on the same thread. The exporter worker observing a malformed batch records on its own thread. No `std::exception_ptr` is moved between threads.
7. **`Shutdown`-safe**: calling any of these methods after `Shutdown` returns silently with a `post_shutdown` drop.

If any of guarantees 1–7 cannot be met for a method, the method is **not** in the public API. It goes on `Provider` (where the lifecycle/structured-status contract applies) or on an internal interface.

---

## 9. Test seams

Three seams exist specifically so tests can drive the threading model deterministically.

### 9.1 `IClock` / `ISteadyClock`

Production injects `std::chrono::system_clock` and `std::chrono::steady_clock`. Tests inject a fake clock that advances on demand. The exporter worker's batch-deadline arithmetic, retry backoff, and timeout enforcement all consume clocks via these interfaces (`memory-model.md` §5.7, `interfaces.md`).

### 9.2 `IReactor`

The transport's epoll/kqueue loop is behind `IReactor`. Tests inject a fake reactor that delivers events on test-driven schedules — useful for verifying GOAWAY mid-batch, RST_STREAM mid-stream, and partial-frame edge cases without an actual socket.

### 9.3 Synchronous-mode `BatchSpanProcessor` (test-only)

The processor exposes a `_test_only_drain_synchronously()` method (named per the suffix rule in `coding-standards.md` §11) that runs one drain cycle on the calling thread. Tests can drive the entire pipeline from the caller thread, with no worker thread, and observe deterministic results. The method is gated behind a test-only header (`include/microtel/internal/test_support.hpp` — added when the test harness lands in M2; not present in M0).

These three seams collectively make every cross-thread contract in this document unit-testable without spinning up real threads.

---

## 10. Thread-safety category for every public type

| Type | Category | Doxygen tag |
|---|---|---|
| `Tracer` | Thread-safe | `@threadsafety Thread-safe` |
| `Span` | Thread-safe **for distinct spans**; **a single `Span` is externally synchronised** | `@threadsafety Externally synchronized` |
| `Provider` | Thread-safe (lifecycle methods may be called from any caller thread) | `@threadsafety Thread-safe` |
| `SdkBuilder` | **Externally synchronised** — caller serialises chained `WithXxx` calls | `@threadsafety Externally synchronized` |
| `Resource` | Immutable after construction; thread-safe for read | `@threadsafety Thread-safe` |
| `LogSink` (callback) | Caller-supplied; microtel makes no thread-safety assumption beyond "may be called from any internal thread" | documented in `log_sink.hpp` |

For every internal interface, the corresponding contract is in `interfaces.md`.

---

## 11. What this document does not cover

- The exact data-structure choice for the span queue (lock-free ring vs mutex queue) — pinned in M3 against benchmarks. The contract here is sufficient for M0.
- The retry timing and backoff algorithm — that's an exporter detail, captured in the retry sequence diagram.
- The drop counter enumeration and the specific diagnostic surface — see `error-model.md`.
- Per-method `@threadsafety` tags below the public API — see `interfaces.md`.
