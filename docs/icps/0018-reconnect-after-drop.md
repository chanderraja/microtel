# ICP 0018: Reconnect after a mid-connection transport drop

**Status:** Draft
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
`"connection lost"`. Both mark the result `retryable`-shaped so the codec's
existing classification does the right thing without a new error path.

This alone converts the gRPC hang into an ordinary retryable failure, which
`OtlpExporter`'s retry loop already handles — and, thanks to §"Reconnect
mostly works already", the retry's `EnsureConnected` re-establishes the
connection.

### 2. Bound the gRPC wait regardless (defence in depth)

`GrpcWireCodec::Send` should use `wait_for(deadline)` like the HTTP codec
rather than an unbounded `get()`. Item 1 removes the known way to wedge it;
item 2 means a future bug cannot re-open the same hole. The `RequestSpec`
already carries a deadline, so the value is to hand.

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

**Recommendation: (a).** The distinction is real and operationally valuable —
"never came up" is a config or network problem, "dropped and recovering" is a
peer problem — and the audit (#134) found that the health surface is already
thinner than documented. Deleting a documented state to match an
unimplemented reality is the wrong direction when the state is cheap to
populate.

### 4. Backoff is out of scope

Reconnect currently rides on the exporter's existing retry loop, which already
has backoff, jitter, and a retry budget (`src/exporter/retry_policy.hpp`).
Adding a *second* backoff inside the transport would compound with it in ways
neither can reason about. If reconnect should be paced independently of export
retry, that is a separate proposal with its own evidence.

## Migration

- No public API change under recommendation (a); `ConnectionState::Reconnecting`
  becomes reachable, which readers of `GetExporterHealth()` should already
  handle since it has always been declared.
- `docs/interfaces.md` §4.1's "Reconnect is internal — clients do not see it"
  becomes true rather than aspirational.
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
3. Should `Close` and the drop path report distinguishable errors to the codec,
   or is the shared `retryable` shape enough? Proposed: same shape, different
   message, since no caller currently branches on the distinction.
