# Sequence: Shutdown Drain

**Status:** M0 deliverable. Normative timeline for `Provider::Shutdown(timeout)`.
**See also:** `threading-model.md` §5, §6, `error-model.md` §2.3, `interfaces.md` §4.4, `microtel-spec.md` §5.3.

---

## Participants

- **Caller thread** — calls `Shutdown(timeout)` and waits for the result.
- **Exporter worker** — drains the queue, completes the in-flight batch.
- **I/O thread** — drains nghttp2 streams, sends GOAWAY, closes the socket.
- **`m_state`** — single atomic shutdown state on the `Provider`.
- **Peer** — receives the GOAWAY and any final DATA frames.

---

## Happy path — completes within timeout

```
Caller        m_state       Exporter Worker        I/O Thread        Peer
  |             |                  |                     |              |
  | Shutdown(timeout=5s)          |                     |              |
  |---------> read NotShutDown    |                     |              |
  |           CAS -> Draining     |                     |              |
  |           wake worker (cv) ----|--> observe Draining               |
  |           wake io (eventfd) ----|----------> observe Draining       |
  |                                |                     |              |
  |                                | drain queue:        |              |
  |                                |   - records pending |              |
  |                                |     are pulled      |              |
  |                                |   - last batch      |              |
  |                                |     encoded                          |
  |                                | Send(last batch) -|->               |
  |                                |                     | HEADERS+DATA-> |
  |                                |                     |<--- 200 ------|
  |                                |<-- WireResult ok -|                  |
  |                                | exit loop                            |
  |                                | join... <-+                          |
  |                                                       |              |
  |                                                       | send GOAWAY ->|
  |                                                       | drain streams |
  |                                                       | close socket  |
  |                                                       | exit loop     |
  |          <-- both threads joined --                                   |
  |          CAS Draining -> Closed                                       |
  |<------ Status::Completed ------                                       |
```

---

## Annotations

1. **Single-source-of-truth shutdown state.** `Provider::m_state` (atomic) is the only writer-controlled signal. It transitions monotonically: `NotShutDown → Draining → Closed`. (LOCKED — `threading-model.md` §5.3.)
2. **Idempotency.** A second concurrent `Shutdown` observes `Draining` and waits for the in-progress shutdown to reach `Closed`, then returns `AlreadyShutDown`. A `Shutdown` on `Closed` returns `AlreadyShutDown` immediately. (LOCKED.)
3. **Worker drain semantics.** On observing `Draining`, the worker:
   - Stops accepting new task notifications (`ForceFlush` requests during shutdown are ignored — they would race the drain).
   - Drains the span queue once.
   - Encodes one final batch if there are records.
   - Calls `Send` with `deadline = remaining timeout`.
   - On completion (success or failure), exits the loop.
4. **I/O thread drain semantics.** On observing `Draining`, the I/O thread:
   - Lets in-flight requests complete (subject to remaining timeout).
   - Sends GOAWAY (`error_code=0`) to the peer.
   - Waits up to a small bounded grace period for in-flight DATA to flush.
   - Closes the socket.
   - Exits the reactor loop.
5. **Both threads must join before `Closed` is published.** The caller's wait blocks on `std::thread::join` for both. The total wait is bounded by `timeout`; if either thread takes too long, the caller observes `Status::TimedOut`.

---

## Variant — `TimedOut`

```
Caller       m_state    Exporter Worker        I/O Thread
  |            |               |                     |
  | Shutdown(1s)                                      |
  |--> Draining                                       |
  |    wake -|->                                       |
  |    wake -|---------------------------> observe ->  |
  |              | drain queue, encode, Send          |
  |              | (peer is slow; per_export ticking) |
  |              |                                    |
  |              | per_export elapses before response |
  |              | RST_STREAM, drop batch              |
  |              | (counted as shutdown_timeout)       |
  |              | exit loop                           |
  |                                                   |
  |                                  (1s elapsed before I/O thread joined)
  |  caller's join waited 1s; not joined yet          |
  |                                                    |
  |  return Status::TimedOut                          |
  |  m_state remains Draining (NOT Closed)            |
  |                                                   |
  |  background: I/O thread eventually completes      |
  |  destructor on Provider waits up to a small       |
  |  bounded grace, then calls Shutdown(small_timeout) |
  |  if needed; eventually m_state -> Closed.         |
```

A `TimedOut` shutdown leaves the system in a defined state: `m_state` is `Draining`; the threads are still running and will eventually reach `Closed` on their own. The destructor of `Provider` invokes a final `Shutdown` with a small bounded timeout (per `microtel-spec.md` §5.3) to ensure cleanup before destruction returns.

If the destructor's final `Shutdown` also times out, the destructor logs a diagnostic and returns. **The destructor never blocks indefinitely.** (LOCKED — `threading-model.md` §6.2.)

---

## Variant — already shut down (idempotent re-call)

```
Caller       m_state
  |            Closed
  | Shutdown(t)
  |---------> read Closed
  |<--- Status::AlreadyShutDown ---
```

No work performed; immediate return.

---

## Variant — concurrent `ForceFlush` and `Shutdown`

`ForceFlush` is observed by the worker as a synthesised batch deadline of "now." If `Shutdown` is invoked while a `ForceFlush` is in-flight:

1. `Shutdown` transitions `m_state` to `Draining`.
2. The worker observes `Draining` *before* completing the flush. The flush returns `Status::Failed` (or `TimedOut`, depending on which signal beat the other to the worker's predicate check).
3. The shutdown drain proceeds.

This is a documented race: applications calling `ForceFlush` immediately followed by `Shutdown` should not assume the flush completed. The recommended pattern is `ForceFlush(d1)`, then `Shutdown(d2)` — the `ForceFlush` returns first, then `Shutdown` runs on a flushed queue.

---

## Records arriving during shutdown

After `m_state` transitions to `Draining`:

- `End()` calls on caller threads observe the state and drop with `post_shutdown` (counted on `IDiagnosticsSink`).
- `StartSpan` continues to return usable `Span` objects (the API never errors on `noexcept` paths). The application's RAII auto-end on those spans will then drop with `post_shutdown`.

This is the expected behaviour: post-shutdown spans are silently dropped, not rejected to the application.

---

## Edge cases captured by tests

- `timeout = 0`: best-effort, returns `TimedOut` immediately if any in-flight work exists.
- Shutdown during active retry — backoff sleep wakes; worker exits the retry loop and exits.
- Shutdown during reconnect — I/O thread observes Draining, abandons reconnect, exits.
- Two concurrent `Shutdown` calls — one returns `Completed`, the other `AlreadyShutDown` (or both observe `Draining` and wait for the same join).
- `Shutdown` from a callback inside a span end — UB at the application level (don't do this), but microtel's drop-and-count keeps it from crashing.
- Destructor on a `Provider` whose `Shutdown` was never called — destructor calls `Shutdown(small_finite_timeout)`.

These live in `tests/integration/lifecycle/` (M3+).
