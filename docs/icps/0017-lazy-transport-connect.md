# ICP 0017: Lazy transport connect, and the interfaces it touches

**Status:** Draft
**Affected interfaces / docs:** [`docs/interfaces.md`](../interfaces.md) §4.3
(`IWireCodec` — precondition amendment, LOCKED, requires this ICP);
`include/microtel/provider.hpp` (`Connect()` Doxygen correction — not a locked
interface, no ICP requirement, included here because it's the other half of
the same contradiction); `docs/threading-model.md` (a stale, unrelated claim
found while researching this — see "Related findings").
**Affected tracks:** Track B/C (wire codecs, `src/wire/http/`,
`src/wire/grpc/`); Track A (`SdkProvider::Connect`, unchanged but re-examined).

## Summary

Make `IWireCodec::Send` transparently connect its bound `ITransport` if it
isn't already connected, instead of requiring the application (or
`microtel::Provider::Connect()`) to have done so first. `Provider::Connect()`
becomes genuinely optional — calling it early is still useful (fail fast at
startup), but skipping it no longer breaks every export. `ITransport`'s own
contract is **unchanged**: `Connect` must still be called and succeed before
`Send`, it's just always the codec that does it now, which the interface
already permits.

## Motivation

**Two locked artifacts already disagree with each other, and neither matches
what ships.** `include/microtel/provider.hpp`'s `Connect()` Doxygen says: *"If
not called, the connection is established lazily on the first export."*
`docs/interfaces.md` §4.1's `ITransport` precondition (signed off, LOCKED)
says the opposite: *"`Connect` must be called and return success before `Send`
is called."* Neither is what the code does — `Http2Transport::Send()` just
fails fast with `"not connected"` if `Connect()` was never called, and nothing
anywhere calls it automatically. Found by reproducing a real failure while
building M17 L5's wire-conformance test (#121): a native-API probe with zero
otel-cpp shim involvement failed identically, ruling out a shim bug before
concluding this was a pre-existing gap.

**It costs the otel-cpp migration path specifically.** `opentelemetry-cpp`'s
own OTLP exporters (gRPC channel, libcurl-based HTTP client) never expose a
manual "connect" step — the channel/client establishes its connection
transparently on the first RPC. A developer migrating from otel-cpp has no
reason to suspect microtel needs an extra step their old SDK never had, and
`Provider::Connect()`'s own doc reinforces the wrong expectation by calling it
optional. The realistic failure mode isn't "someone skipped a documented
step," it's "an experienced otel-cpp developer had no reason to know the step
exists" — silent, until they read `GetExporterHealth()` or step through the
SDK, which is what it took to find this. Fixing only the doc (say "Connect is
required") is honest but leaves that exact friction in place for the
migration this project's own Tier-3 shim exists to serve.

**`IWireCodec`'s own precondition is the one that actually has to move.**
Both `ITransport` (§4.1) and `IWireCodec` (§4.3) currently state "already
connected" as a precondition of `Send`. The natural place to absorb "connect
if needed" is `IWireCodec`, not `ITransport`: every `IWireCodec` impl
(`HttpWireCodec`, `GrpcWireCodec`) already holds the `ITransport*` it sends
through (`src/wire/http/http_wire_codec.hpp:89`), so no new dependency is
needed, and `OtlpExporter`'s retry loop already treats `IWireCodec::Send` as a
single blocking, deadline-bounded call (`m_config.export_deadline`) — adding
one more blocking sub-step (connect, bounded by its own existing
`TimeoutOptions::connect`) before the existing blocking send doesn't introduce
a new blocking shape, it extends the one already there. `ITransport::Send`'s
own postcondition — "returns immediately with a handle" — stays true
unconditionally, because the connect happens in the codec, one layer up, not
inside `Http2Transport::Send` itself.

## Proposed change

### 1. `IWireCodec::Send` ensures a connection before sending

`HttpWireCodec::Send` / `GrpcWireCodec::Send`, at the top of their existing
implementation, before doing anything else:

```cpp
if (m_transport->GetState() != ConnectionState::Connected)
{
    auto connected = m_transport->Connect(m_connect_opts);
    if (!connected)
    {
        return WireResult{.success = false, .retryable = true, /* ... */};
    }
}
// existing Send() body, unchanged, from here down.
```

This is the entire behavioral change. Everything downstream of it — retry
classification, backoff, `GetExporterHealth()` — is existing, untouched
machinery, which is the point: a failed connect attempt becomes an ordinary
`retryable = true` `WireResult`, indistinguishable in shape from any other
transport-level failure the retry loop already knows how to handle
(`src/wire/http/http_wire_codec.cpp:244` already sets `retryable = true`
unconditionally for transport failures — today's "not connected" error is
*already* going through that exact path; this proposal is what makes the
first attempt succeed instead of being the only attempt).

### 2. Concurrent first-export races are resolved by `Connect`'s existing CAS, not new synchronization

One `Http2Transport` is shared across all three signals' exporters
(`src/sdk/sdk_builder.cpp:312`), each with its own `IWireCodec` instance and
its own exporter worker thread. If traces, metrics, and logs all attempt their
first export around the same time (the common case — process startup),
multiple codecs may call `Connect()` on the same shared transport
concurrently. No new locking is proposed: `Http2Transport::Connect`'s existing
`compare_exchange_strong(Disconnected → Connecting)` guard
(`http2_transport.cpp:438`) already ensures exactly one caller performs the
handshake; the others receive an `"already connecting"` error, which becomes
`retryable = true` the same as any other failure, and their own retry loop
(first retry has no delay per existing `RunRetryLoop` behavior, then
exponential backoff — `src/exporter/retry_policy.hpp`) picks them up shortly
after, by which point the winner has very likely finished. Worst case this
costs the losing codecs one wasted attempt at startup, not a correctness
issue and not new behavior — concurrent `Connect()` calls already resolve
this way today, whenever an application calls `Provider::Connect()` from
multiple threads.

### 3. `Provider::Connect()` stays, becomes genuinely optional

No change to `SdkProvider::Connect()` or the public `Provider::Connect()`
signature. Calling it early remains useful — an application that wants
fail-fast-at-startup semantics (many operators prefer this to
silently-degraded telemetry) still gets that by calling it explicitly and
checking the result. What changes is that *not* calling it no longer breaks
every export; the first `Send()` on each signal connects transparently
instead.

### 4. `Provider::Connect()`'s Doxygen becomes true

Its comment already says the right thing (*"established lazily on the first
export"*) — today's bug is that it's aspirational. After this change it's
accurate. No wording change needed beyond, perhaps, a forward reference to
`IWireCodec` for anyone who wants the mechanism, not just the promise.

### 5. Interface amendment: `docs/interfaces.md` §4.3

`IWireCodec`'s precondition changes from:

> The underlying `ITransport` is connected.

to:

> If the underlying `ITransport` is not connected, `Send` connects it first
> (see ICP 0017). A failed connect attempt is reported as an ordinary
> `retryable` `WireResult`, not a distinct error shape.

`ITransport` §4.1 is **not amended** — its precondition ("`Connect` must be
called and return success before `Send` is called") remains true; the codec
is simply always the one satisfying it now; nothing about `ITransport`'s own
contract changes.

## Migration

- No call-site changes for applications already calling `Provider::Connect()`
  explicitly — it still works exactly as before.
- Applications that never called it (the otel-cpp-migration common case) go
  from "every export fails" to "it just works," with no code change on their
  part.
- `HttpWireCodec` / `GrpcWireCodec` gain the check-and-connect prologue;
  their constructors already receive everything needed (`ITransport*`,
  `ConnectOptions`) — confirm `ConnectOptions` is already threaded to both, or
  add it as a constructor parameter if not (implementation detail, not a
  contract change).
- Three existing tests assert today's fail-fast shape and need to change
  (found in the same investigation this ICP is based on):
  `tests/unit/transport/http2_transport_test.cpp`'s `Send_WhenDisconnected_FutureHasError`,
  `Send_WhenDisconnected_HandleIdIsZero` exercise **`Http2Transport::Send`
  directly**, below `IWireCodec` — those are unaffected, since `ITransport`'s
  contract doesn't change. `Send_WhenClosed_FutureHasError` also exercises the
  transport layer directly and is unaffected. **No `ITransport`-level test
  needs to change.** New tests are needed instead, at the `IWireCodec` level:
  a codec `Send()` call while disconnected now succeeds (given a reachable
  endpoint) rather than failing, and a codec `Send()` call against an
  unreachable endpoint now surfaces the connect failure as a normal
  `retryable` result.

## Rationale & alternatives

- **Put the lazy-connect inside `Http2Transport::Send` itself** — rejected.
  This is the design I initially assumed before researching the locked
  interfaces. It requires amending `ITransport`'s own precondition *and*
  reconciling a blocking connect with `Send`'s locked postcondition ("returns
  immediately with a handle") — either accepting an undocumented exception to
  that postcondition for the first call, or making the in-`Send` connect
  itself asynchronous (queue the request, resolve it once connected), which
  is real new async-state-machine work for no benefit over doing it one layer
  up, where `IWireCodec::Send` is already a blocking call as far as its only
  caller (`OtlpExporter`) is concerned.
- **Connect eagerly inside `SdkBuilder::Build()`** — rejected. Simpler, but
  loses the deferred-cost property the "lazy" wording asks for: a `Provider`
  that's built but never actually used for export (feature-flagged telemetry,
  certain test constructions) would pay a network handshake it doesn't need.
  It also doesn't match what any real OTLP SDK — including the
  `opentelemetry-cpp` this shim exists to be compatible with — actually does.
- **Do nothing beyond fixing the Doxygen** — rejected per the Motivation
  section above: it's honest, but leaves exactly the friction point this
  project's Tier-3 shim exists to remove for the otel-cpp migration audience.

## Related findings (out of scope for this ICP)

Found while researching this proposal; each is real but independent of the
lazy-connect question, and shouldn't be bundled into this diff:

1. **`ConnectionState::Reconnecting` is dead code.** No codepath sets or
   reads it. On an I/O error mid-connection, `Http2Transport::OnIoEvent`
   drops straight `Connected → Disconnected`
   (`http2_transport.cpp:964,972`) with **no cleanup** — no `Unregister` of
   the dead fd from the reactor, no socket close, no session reset — and
   nothing reconnects automatically afterward. `docs/interfaces.md` §4.1's
   own invariant ("Reconnect is internal — clients do not see it") is
   currently unimplemented for the *post-drop* case; this ICP only addresses
   the *initial* connect. Worth its own ICP: a transparent reconnect-after-
   drop is a materially different problem (detecting the drop, deciding when
   to retry, not resource-leaking the dead connection first).
2. **`docs/threading-model.md` §2.3 is factually wrong today**, independent
   of anything proposed here: it states the I/O thread is *"Created at
   `Transport::Connect`; joined at `Transport::Close`."* It's actually
   created in `Http2Transport::Create()` (`http2_transport.cpp:406`), before
   any `Connect()` call — confirmed by the fact that `IoThreadLoop` is
   already polling (gated on `IsValid()`) whether or not a connection exists.
   A one-line doc fix, unrelated to this proposal's mechanism, but the kind
   of drift this ICP's research surfaced and shouldn't get lost.

## Open questions for the reviewer

1. **Does a lazy-connect failure deserve its own `DropReason` / health
   signal**, or is the existing `GetExporterHealth().connection_state` /
   `last_error_message` surface sufficient? Given ICP 0016's precedent
   (new `DropReason` values needed a `Provider::RecordAdapterDrop` access
   path because the failure happens above the SDK), this failure happens
   *inside* the SDK's own codec, where `IDiagnosticsSink` is already
   reachable — likely no new plumbing needed, just confirming which existing
   counter increments. Not resolved here.
2. **Should the very first connect attempt's failure be surfaced any
   differently from a later one** (e.g., a distinguishable "never
   successfully connected" vs. "was connected, then dropped" state), given
   related finding #1 above means the two cases currently look identical
   (`Disconnected`) despite being operationally different? Deferred to
   whichever ICP eventually tackles reconnect-after-drop.
