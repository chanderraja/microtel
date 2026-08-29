# ICP 0019: Perform `Connect` on the I/O thread

**Status:** Draft
**Depends on:** #153 (`ConnectionState::Reconnecting`), merged — it introduces
the `ClaimConnectSlot` helper and the `Disconnected|Reconnecting → Connecting`
claim this proposal builds on.
**Affected interfaces / docs:** [`docs/interfaces.md`](../interfaces.md) §4.1
(`ITransport` threading contract — LOCKED, requires this ICP);
[`docs/threading-model.md`](../threading-model.md) §2.3.
No public API change.
**Affected tracks:** Track D (`src/transport/`).

## Summary

Move the body of `Http2Transport::Connect` onto the I/O thread, so that every
access to the connection objects — `m_socket`, `m_ssl_ctx`, `m_ssl_session`,
`m_nghttp2_session` — happens on one thread. The caller enqueues a connect
request, wakes the reactor, and waits on a future bounded by
`ConnectOptions::connect_timeout`.

This removes a class of data race rather than managing it. It also restores a
claim `threading-model.md` §2.3 made from M0 and that the code never satisfied.

## Motivation

### The concrete race

Implementing `ConnectionState::Reconnecting` (#153) made reconnect a
first-class path, and TSAN found a data race on the first run of the new
integration test: `IoThreadLoop` read `m_nghttp2_session` while `Connect`, on
another thread, was writing it.

#153 closed that specific window by gating the loop on `m_state`, matching
`OnIoEvent`'s existing acquire-load. **That is publication, not exclusion.**
`m_state` release/acquire guarantees a reader that observes `Connected` sees
the writes that preceded it. It guarantees nothing about a reader already past
its check when a *new* connect begins writing. The window is narrow and TSAN
no longer flags it, but it is narrow by timing, not closed by construction.

### The access pattern makes this tractable

Every touch of the four connection objects, traced against `master`:

| Accessor | Thread | Kind |
|---|---|---|
| `Connect` (`m_socket = …`, `m_nghttp2_session = …`, TLS handshake, `SslConnectLoop`, `poll`) | **caller, or an exporter worker** since ICP 0017 | **write** |
| `Close` (`Unregister`, `Reset`, `Close`) | caller, **after the I/O thread is joined** | write |
| `IoThreadLoop`, `OnIoEvent` | I/O | read |
| `DrainPendingRequests` → `SubmitStream`, `DrainCancelQueue` | I/O | read |
| `NgHttp2DoSend`, `NgHttp2DoRecv` (nghttp2 callbacks) | I/O | read |

`Send` does not appear: it checks `m_state` and pushes onto `m_pending_queue`.

So **there is exactly one cross-thread writer — `Connect`** — and every reader
is already on the I/O thread. `Close` is already safe because it runs after the
join. Moving that single writer onto the I/O thread makes all access to these
objects single-threaded, and the synchronisation question stops existing.

### It restores a documented claim

`threading-model.md` §2.3 said the I/O thread **owns** the `SslCtx`,
`SslSession`, `Nghttp2Session` and socket. #149 had to weaken that to "Reads",
with a note that they are constructed by `Connect` on another thread, because
that is what the code does. Under this proposal the original sentence becomes
true again — evidence that this was the intended design before ICP 0017 made
`Connect` reachable from exporter workers.

## Proposed change

Reuse the mechanism `Send` already uses.

1. **A connect-request queue.** `ConnectRequest { ConnectOptions opts;
   std::promise<Expected<void, Error>> promise; }`, guarded by a small mutex,
   drained by the I/O thread exactly as `DrainPendingRequests` drains
   `m_pending_queue`.

2. **`Connect` becomes a submit-and-wait.** It performs the existing
   `Disconnected|Reconnecting → Connecting` claim via `ClaimConnectSlot`,
   enqueues the request, calls `Wake()`, and waits on the future with
   `opts.connect_timeout`.

   **On timeout the caller returns the error and touches no state.** An earlier
   draft of this ICP said it "restores the prior state", which reads as
   harmless and is not: the I/O thread may be mid-connect at that moment and
   will go on to write `m_socket` and `m_nghttp2_session` and store
   `Connected` — *after* the caller has already stored `Disconnected` and
   returned failure. The state would then claim disconnected while a live
   connection existed, and the next `Connect` would win the claim and overwrite
   those objects, closing a socket the I/O thread had just registered.

   That is the one place where relocating the work would otherwise *create* a
   hazard instead of removing one, and it is the same principle this ICP is
   built on, applied to the caller: **the I/O thread owns every `m_state`
   transition out of `Connecting`, including the abandoned one.** The caller
   only ever reports. A late success is then simply a connection that is up
   when the caller believed it was not, which the next `Send` handles correctly
   since ICP 0017's lazy connect.

3. **The I/O thread performs the connect** — DNS, TCP, TLS, SETTINGS,
   `Register` — using today's code, unchanged, simply relocated. All writes to
   the four objects now happen there.

4. **Readers lose their guard.** The `m_state` gate #153 added to
   `IoThreadLoop` becomes unnecessary. Keeping it is harmless and it documents
   intent, so it should stay, but it stops being load-bearing.

5. **The I/O thread's exit path must fulfil every queued connect request.**
   If the thread exits — `Close`, or an unrecoverable loop error — between an
   enqueue and its drain, that `std::promise` is never set and `Connect` waits
   out its full `connect_timeout` on a future nobody will fulfil. Bounded, so
   not ICP 0018's indefinite hang, but ten seconds of a caller blocked for no
   reason. `ClaimConnectSlot`'s `Closed` arm does not help: the claim already
   succeeded before the thread died.

   This is the same bug class ICP 0018 fixed for stream promises, and the
   remedy is the same shape — the loop's exit path abandons queued connect
   requests with an error, exactly as `Close` calls `AbandonInFlight` for
   in-flight streams. Naming it here rather than leaving it to implementation,
   because it is the kind of path that only shows up under a race and is easy
   to omit.

`Close` is otherwise untouched: it already runs after the join.

## What this costs

Stated plainly, because it is a real trade and not a free win.

- **The I/O thread blocks for the whole connect.** DNS resolution, TCP connect,
  TLS handshake and the SETTINGS exchange are all synchronous. During a
  *reconnect* there is no live connection to service, so little is lost — but
  `DrainCancelQueue` stalls, so a codec that times out mid-reconnect will not
  have its cancel processed until the connect completes or times out. Bounded
  by `connect_timeout` (default 10 s).
- **Blocking DNS on an I/O thread is normally a mistake.** It is acceptable
  here only because this thread services exactly one connection. If the
  transport ever multiplexes endpoints — the v2 leaf/concentrator direction —
  this stops being true and the design must be revisited. Worth recording as a
  constraint rather than discovering it later.
- **`Connect` gains a cross-thread round trip**, so it now depends on the I/O
  thread being alive. After `Close` the thread is gone; `Connect` must fail
  fast rather than wait out its timeout. The `Closed` arm of `ClaimConnectSlot`
  already rejects that case before anything is enqueued, so this is a matter of
  keeping the ordering, not new logic.

## Rationale & alternatives

- **A mutex over the four objects** — rejected, and it is the obvious first
  idea. The nghttp2 callbacks perform `read()`/`write()` syscalls *inside*
  `nghttp2_session_send/recv`, so the lock would be held across I/O, violating
  `threading-model.md` §8's LOCKED "no mutex held for unbounded duration". It
  would also nest with `m_pending_mu` and `m_cancel_mu`, violating §4's LOCKED
  single-non-leaf-lock rule — the rule #146 has just finished restoring.

- **`std::atomic<std::shared_ptr<Connection>>`** — a genuine alternative, and
  the right fallback if blocking the I/O thread proves unacceptable. Bundle the
  four objects into an immutable `Connection`; readers load into a local
  `shared_ptr`, writers construct a new one and swap; the old connection dies
  when the last reader releases it. Airtight, moves no work, and keeps connect
  off the I/O thread. Costs a refcount round trip per read — acceptable, since
  this is the I/O path and not the span hot path — and makes lifetime reasoning
  subtler, because the nghttp2 callbacks hold a raw `this` and would need the
  session kept alive for the duration of a `recv`/`send` call. More machinery
  for the same guarantee.

- **Leave it as #153 left it** — rejected, but not unreasonable. TSAN is clean
  and the window is small. It is rejected because the guarantee is timing-based
  rather than structural: the next change to the reconnect path re-opens it,
  and the failure mode is a torn read of an OpenSSL or nghttp2 handle, which
  will not reproduce and will not be diagnosable from a bug report.

## Migration

- No public API change. `Connect`'s signature, return type, and error values
  are unchanged.
- `interfaces.md` §4.1's threading contract for `Connect` is amended: it is
  currently "caller-thread-safe … called from the exporter worker on first use
  (ICP 0017)". It becomes: callable from any thread; the work is performed on
  the I/O thread and the caller blocks on the result.
- `threading-model.md` §2.3's "Reads" reverts to "Owns", with the ICP 0017
  note removed.
- Tests: existing connect and reconnect integration tests should pass
  unchanged — that is the acceptance criterion. Add one for connect-after-Close
  failing fast rather than blocking for `connect_timeout`.

## Resolved at review

1. **Is blocking the I/O thread for the connect duration acceptable? — Yes.**
   The concession that matters is `DrainCancelQueue` stalling, and it is
   smaller than it first appears: during a reconnect there is no live
   connection, so the requests whose cancels are stalled are ones that cannot
   make progress anyway. The codec times out, the cancel lands late, the stream
   state is cleaned up when the connect finishes. Nothing is lost but
   promptness, on a path where promptness has no consumer.

   `atomic<shared_ptr>` remains correctly identified as airtight and correctly
   rejected on machinery. The deciding property is that **single-threaded
   access is right for a new contributor who does not understand why** — the
   `shared_ptr` design's subtlety (nghttp2 callbacks holding raw `this` across
   a `recv`/`send`) is exactly the kind that produces an unreproducible bug
   two years later.

2. **Should `Provider::Connect()` stay synchronous? — Yes.** Returning on
   *enqueue* would have it report success for a connect that has not happened,
   which is precisely the failure ICP 0017 was written to eliminate.

3. **Should this wait for the v2 leaf/concentrator design? — No, but record
   the expiry.** Blocking a correct fix on a design that does not exist yet,
   in service of an architecture that may not survive contact with M7's
   numbers, is the wrong trade. And if v2 does multiplex endpoints,
   `atomic<shared_ptr>` is a contained change to the same four objects, not a
   rewrite.

   **The implementation must carry the expiry condition as a comment at the
   connect-request drain site**, not only here. A v2 designer adding a second
   endpoint will read that code; they will not necessarily read this ICP. The
   comment should say plainly that performing the connect inline is safe only
   while this thread services exactly one connection, and that multiplexing
   invalidates it.

## Open questions

None outstanding. Two implementation requirements — the exit-path drain
(Proposed change §5) and the caller-touches-no-state rule on timeout
(§2) — came out of review and are recorded there rather than left open,
because both are correctness properties rather than preferences.
