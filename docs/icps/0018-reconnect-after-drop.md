# ICP 0018: Reconnect after a mid-connection transport drop

**Status:** Draft — items 1 and 2 implemented in #151; §3 and open question 1 still open
**Affected interfaces / docs:** [`docs/interfaces.md`](../interfaces.md) §4.1
(`ITransport` — the "Reconnect is internal" invariant, currently unimplemented);
`include/microtel/provider.hpp` (`ConnectionState::Reconnecting`, dead since M0);
[`docs/threading-model.md`](../threading-model.md) §3.3.
**Affected tracks:** Track D (`src/transport/`), Track B/C (wire codecs).

## Summary

`docs/interfaces.md` §4.1 states *"Reconnect is internal — clients do not see
it."* Nothing implements it. This ICP proposes what to do about that — and the
answer is smaller than it first appears, because **ICP 0017 accidentally
delivered most of the reconnect already**. What is missing is the cleanup and
promise-fulfilment on the drop path, and that gap currently **hangs a gRPC
exporter thread forever**.

## What actually happens today

Verified against `master`, not against the docs.

### Reconnect mostly works already, by accident

On an I/O error the transport stores `Disconnected`
(`http2_transport.cpp:986`, `:994`). `Connect`'s guard is a CAS from
`Disconnected → Connecting` (`:441`), which therefore **succeeds** after a
drop. Since ICP 0017, `IWireCodec::Send` calls `Connect` whenever the state is
not `Connected` — so the next export after a drop already re-establishes the
connection.

That was not designed; it falls out of the drop storing the one state the CAS
accepts. It is worth writing down before someone "fixes" the CAS and silently
removes the only reconnect the project has.

Resource replacement on that path is also better than it looks:
`m_socket = std::move(*fd)` (`:475`) closes the previous socket through
`UniqueFd`'s move-assignment, and closing an fd removes it from the epoll set.
`m_ssl_ctx`, `m_ssl_session`, and `m_nghttp2_session` are likewise
move-assigned over. So the fd and TLS objects are **not** leaked across a
reconnect.

### What is genuinely broken

**1. In-flight requests are abandoned, and gRPC hangs on it.**

`Close` explicitly fulfils every pending promise with a `Cancelled` error
(`http2_transport.cpp:539-547`). The drop path does not — it stores
`Disconnected` and returns, leaving every entry in `m_streams` with an
unfulfilled `std::promise`.

The two codecs then diverge:

| Codec | Wait | Behaviour on drop |
|---|---|---|
| HTTP | `fut.wait_for(deadline)` (`http_wire_codec.cpp:253`) | Times out, cancels, returns `retryable` — recovers |
| gRPC | `handle.Future().get()` (`grpc_wire_codec.cpp:762`) | **Blocks forever** |

An unbounded `get()` against a promise nobody will ever set is a permanently
wedged exporter worker. `Provider::Shutdown` then blocks on joining it (until
#144's timeout reports `TimedOut`, which does not unwedge the thread). This is
the same shape as the gRPC retry gap fixed in #138: the HTTP path was written
defensively and the gRPC path was not.

**2. `m_streams` and `m_handle_to_stream` grow across drops.** Their entries
are cleared in `Close` but not on the drop path, so each drop leaks the
per-stream state of whatever was in flight.

**3. `ConnectionState::Reconnecting` has never been used.** It is declared
(`provider.hpp:34`) and referenced nowhere in `src/`, `include/`, or `tests/`.
`GetExporterHealth().connection_state` therefore cannot distinguish "never
connected" from "was connected, dropped, and is re-establishing" — the two
cases an operator most wants to tell apart, and the open question ICP 0017
explicitly deferred here.

## Proposed change

### 1. Fulfil and clear on the drop path (required, fixes the hang)

Extract the fulfil-and-clear block that `Close` already runs into a helper, and
call it from the drop path in `OnIoEvent`:

```cpp
void Http2Transport::AbandonInFlight(microtel::Error::Kind kind, const char* why) noexcept;
```

`Close` keeps its current message (`"transport closed"`); the drop path uses
`"connection lost"`.

**Both paths pass `Error::Kind::Cancelled`, and that is safe — but for a
reason worth pinning down, because it is incidental rather than designed.**
Neither codec branches on `Error::Kind`. `GrpcWireCodec::ClassifyResponse`
tests `tr.error.has_value()` and `HttpWireCodec::Send` tests `!result.success`;
both then return `retryable = true` unconditionally for transport-level
failures. So a drop is retried, which is what we want.

The hazard this avoids is real: if either codec were changed to treat
`Cancelled` as non-retryable — a defensible reading, since a cancelled request
genuinely should not be retried — item 1 would convert the hang into a
**silent permanent failure**, which is worse than the hang because nothing
surfaces it. Any future change to error classification must keep drop-path
errors retryable, or give the drop path its own `Kind`.

For `Close`, retryability is moot: the provider is shutting down, so the retry
loop does not run.

Implementation note (#151): the helper takes only a message, not a `Kind`,
precisely because both callers want `Cancelled` today. Splitting them is the
change to make if the classification above ever moves.

This alone converts the gRPC hang into an ordinary retryable failure, which
`OtlpExporter`'s retry loop already handles — and, thanks to §"Reconnect
mostly works already", the retry's `EnsureConnected` re-establishes the
connection.

### 2. Bound the gRPC wait regardless (defence in depth)

`GrpcWireCodec::Send` should use `wait_for(deadline)` like the HTTP codec
rather than an unbounded `get()`. Item 1 removes the known way to wedge it;
item 2 means a future bug cannot re-open the same hole. The `RequestSpec`
already carries a deadline, so the value is to hand.

**Ordering hazard, checked.** A timed-out codec calls `Cancel` while the I/O
thread may be abandoning the same stream. Three properties make that safe, and
all three are load-bearing enough to state rather than rediscover:

1. **`Cancel` never fulfils.** It pushes the handle id onto `m_cancel_queue`
   under `m_cancel_mu` and wakes the reactor. All fulfilment happens on the I/O
   thread.
2. **Everything that touches `m_streams` runs on the I/O thread.**
   `AbandonInFlight` (from `OnIoEvent`), `DrainCancelQueue`, and
   `FulfillStream` are all reached from `IoThreadLoop`, so they are serialised
   with no lock needed. `Close`'s call runs after the join.
3. **Neither path can double-fulfil.** `FulfillStream` erases the entry before
   setting its promise, so any entry still in the map has an unset promise;
   `DrainCancelQueue` looks the handle up and `continue`s when it is gone.
   After `AbandonInFlight` clears the maps, a queued cancel for the same
   request finds nothing and does nothing.

A double `set_value` would throw `std::future_error` out of a `noexcept`
frame and terminate, so this is not a theoretical tidiness point.

### 3. Decide `Reconnecting`

Two options, and this is the part that most needs a reviewer's call:

- **(a) Implement it.** The drop path stores `Reconnecting` instead of
  `Disconnected`, and `Connect`'s CAS accepts `Reconnecting → Connecting` as
  well. `GetExporterHealth` then distinguishes the two cases. Cost: the CAS
  gains a second accepted state, and every reader of `connection_state` must
  handle a value it has never seen.
- **(b) Delete it.** Remove the enumerator, and treat `Disconnected` as
  covering both. Cost: it is a public enum, so removing a value is a breaking
  change requiring its own ICP note; and operators lose a distinction the
  documentation has promised since M0.

**Recommendation: (a)**, and the decisive argument is compatibility, not
operator ergonomics.

**`Reconnecting` is already a public enumerator.** Any consumer writing an
exhaustive `switch` on `ConnectionState` already has a `Reconnecting` arm;
anyone who wrote a non-exhaustive one has a latent bug today, independent of
this ICP. **Populating the value is therefore not a compatibility event.
Deleting it is** — and it would be the second breaking change to the health
surface, which is precisely the part of the API operators build alerting
against.

The operator argument is real but secondary: "never came up" is a config or
network problem, "dropped and recovering" is a peer problem, and the audit
(#134) found the health surface already thinner than documented.

### 4. Backoff is out of scope

Reconnect currently rides on the exporter's existing retry loop, which already
has backoff, jitter, and a retry budget (`src/exporter/retry_policy.hpp`).
Adding a *second* backoff inside the transport would compound with it in ways
neither can reason about. If reconnect should be paced independently of export
retry, that is a separate proposal with its own evidence.

## Migration

- No public API *shape* change under recommendation (a);
  `ConnectionState::Reconnecting` becomes reachable, which readers of
  `GetExporterHealth()` should already handle since it has always been
  declared.
- **But there is a behavioural change, and calling it "no public API change"
  would hide it.** Consumers that branch on `Disconnected` will see *fewer*
  `Disconnected` transitions once (a) lands, because a mid-connection drop
  produces `Reconnecting` instead. Nobody depends on that yet — the state
  machine has never emitted `Reconnecting` — but an alerting rule keyed on
  `Disconnected` would silently stop firing for drops, which is exactly the
  kind of change that should be announced rather than discovered.
- `docs/interfaces.md` §4.1's "Reconnect is internal — clients do not see it"
  becomes **mostly** true rather than aspirational, and the sentence should be
  sharpened while it is being made real. Clients *do* observe reconnect: through
  `connection_state` transitions, and through the retryable `WireResult` on the
  export that triggers it. What the sentence actually means is that there is
  **no client-initiated reconnect API** — no `Reconnect()` to call, no
  reconnect policy to configure. Proposed wording:

  > Reconnect is internal: there is no client-initiated reconnect call. Clients
  > observe it only indirectly, through `connection_state` and through the
  > retryable export failure that triggers it.

  Stating it precisely matters here because the imprecise version is what let
  the invariant go unimplemented for so long — "clients do not see it" reads as
  a property nobody can test.
- Tests: a drop with in-flight requests must fulfil them (both codecs); a gRPC
  `Send` must not block past its deadline; a reconnect after a drop must
  succeed on the next export.

## Rationale & alternatives

- **Reconnect on a timer inside the transport** — rejected. Duplicates the
  exporter's retry loop and would reconnect for a `Provider` that has stopped
  exporting entirely.
- **Fail permanently on drop and require `Provider` rebuild** — rejected. That
  is closer to today's behaviour and is exactly what §4.1 promises against.
- **Fix only the gRPC unbounded `get()`** — rejected as insufficient. It
  converts a hang into a per-request timeout, but leaves every in-flight
  request waiting out its full deadline on every drop, and leaves `m_streams`
  growing.

## Open questions for the reviewer

1. §3: implement `Reconnecting` (recommended) or delete it?
2. Should a drop increment a `DropReason`? `ConnectFailure` exists and has no
   increment site (one of the 22 dead counters in #134); a drop is arguably a
   different event. Deciding this alongside #134's counter work rather than
   inventing a reason here.
3. **Resolved: same shape, different message — and the distinction matters
   more than the original framing suggested.** The proposal justified it by
   saying no caller branches on it. That is true of the *codec*, and it is the
   wrong consumer to reason about: the message lands in
   `HealthSnapshot::last_error_message`, where `"transport closed"` versus
   `"connection lost"` is the difference between an operator seeing an orderly
   shutdown and an unexpected peer drop. Since the two messages already
   differ, the distinction is free.

   Recorded so that nobody later "simplifies" the two call sites into one
   shared message on the grounds that the codec cannot tell them apart. **The
   health surface is the consumer, not the codec.**
