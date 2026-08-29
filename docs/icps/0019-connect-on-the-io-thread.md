# ICP 0019: Perform `Connect` on the I/O thread

**Status:** Draft
**Depends on:** #153 (`ConnectionState::Reconnecting`), which introduces the
`ClaimConnectSlot` helper and the `Disconnected|Reconnecting → Connecting`
claim this proposal builds on. At the time of writing #153 is in review, not
merged.
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
   `Disconnected|Reconnecting → Connecting` claim via `ClaimConnectSlot`
   (introduced by #153 — see Depends on), enqueues the request, calls `Wake()`, and waits on the future with
   `opts.connect_timeout`. On timeout it restores the prior state and returns
   the existing timeout error.

3. **The I/O thread performs the connect** — DNS, TCP, TLS, SETTINGS,
   `Register` — using today's code, unchanged, simply relocated. All writes to
   the four objects now happen there.

4. **Readers lose their guard.** The `m_state` gate #153 added to
   `IoThreadLoop` becomes unnecessary. Keeping it is harmless and it documents
   intent, so it should stay, but it stops being load-bearing.

`Close` is untouched: it already runs after the join.

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

## Open questions for the reviewer

1. **Is blocking the I/O thread for the connect duration acceptable?** It is
   the crux. If the answer is no, the alternative above (`atomic<shared_ptr>`)
   achieves the same safety without moving the work, at the cost of more
   machinery.
2. **Should `Provider::Connect()` keep its synchronous signature?** It could
   return once the request is *enqueued* rather than completed, which would
   remove the round trip — but it would also break the fail-fast-at-startup
   semantics `Provider::Connect()` exists to provide (ICP 0017 §3). Proposed:
   keep it synchronous.
3. Does this want to wait for the v2 leaf/concentrator design? If the transport
   is going to multiplex endpoints, the single-connection assumption underneath
   this proposal expires, and `atomic<shared_ptr>` becomes the better long-term
   shape.
