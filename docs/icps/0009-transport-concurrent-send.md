# ICP 0009 — `ITransport::Send` is safe for concurrent callers

**Status:** Accepted — signed off 2026-08-30, **retroactively**. The relaxation
this ICP proposed shipped with M12: `SdkBuilder::Build` has built three codecs
over one transport since then, each driven by its own exporter worker, so
`Send` has had concurrent callers in every build. The ICP was never accepted
and its changes were never applied, leaving `interfaces.md` §4.1 asserting the
opposite — LOCKED — for the whole period. Documentation applied and the
concurrent-`Send` TSAN test written in the PR that carries this line.
**Author:** Chander Raja
**Affected interfaces / docs:** `include/microtel/internal/transport.hpp`
(`ITransport::Send` threading contract); `docs/threading-model.md` §2.2 / §3.2
(+ lock table §4); `src/transport/README.md`
**Affected tracks:** Track D — Transport (owns `ITransport` / `Http2Transport`);
consumed by the M12 metrics export pipeline (`docs/metrics-design.md` §5).

## Summary

Relax `ITransport::Send` from "single-caller — only the exporter worker may
call" to "**safe for concurrent callers; the transport serializes submission
onto its single I/O thread**," so the v1.2 metrics exporter can share one
transport (and one socket / nghttp2 session) with the span exporter.

## Motivation

v1 has exactly one `Send` caller: the span exporter worker thread. v1.2 (M12)
adds a **second** exporter worker for metrics, and `docs/metrics-design.md` §5
mandates the metrics pipeline **share the existing transport connection** (the
two signals have different lifecycles but one socket — HTTP/2 multiplexing is
exactly what makes this work). Two worker threads calling `Send` on one
transport violates the current LOCKED single-caller contract.

`Http2Transport::Send` already enqueues into `m_pending_queue` under
`m_pending_mu` and allocates handle ids atomically — it is **mechanically
MPSC-safe today** — but the documented contract and the threading audit/TSAN
gates assume a single producer. This ICP makes the already-safe behavior the
contract, rather than relying on incidental thread-safety.

## Proposed change

- **`transport.hpp`** — change `Send`'s `@threadsafety` from "Single-threaded;
  only the exporter worker may call" to: *"Thread-safe for submission — any
  number of caller threads may call `Send` concurrently; the transport
  serializes submissions onto its single I/O thread. Each caller still owns its
  own `IWireCodec`; codecs and `IOtlpEncoder` remain single-caller."*
- **`threading-model.md` §2.2 / §3.2** — the worker→I/O submission queue becomes
  **MPSC** (multiple producer worker threads, single I/O-thread consumer). Update
  the §4 lock table: `m_pending_mu` is a leaf lock any submitting thread may take;
  no caller holds another non-leaf lock while calling `Send` (codec/exporter
  `m_mu` is released before `Send`, per the existing drain-to-empty pattern).
- **`Http2Transport`** — confirm/harden that every field touched by
  `Send` / `Cancel` / `DrainPendingRequests` is mutex- or atomic-guarded
  (`m_pending_queue`, `m_cancel_queue`, `m_next_handle_id` already are). Add a
  TSAN integration test: two threads issue `Send` concurrently against the
  loopback server; assert both complete and responses route by handle id.

## Migration

- **No public API signature change** — this is a threading-contract widening plus
  doc/test updates. Existing callers need no change.
- Existing single-caller code remains valid (a stricter usage of a now-relaxed
  contract). The span exporter is untouched.
- The M12 metrics exporter may construct its own `IWireCodec` over the shared
  transport and call `Send` from its own worker thread.

## Rationale & alternatives

- **Alternative B — a Provider-owned send-mutex** wrapping both codecs' `Send`,
  leaving the single-caller contract literally intact. Rejected as primary: adds
  a cross-pipeline lock and serializes span/metric round trips, muddying the
  clean per-pipeline ownership — for no real gain, since `m_pending_mu` already
  provides safe MPSC enqueue and HTTP/2 multiplexes both signals over one
  session. **Kept as the fallback** if hardening + TSAN validation slips.
- **Alternative C — a second transport/connection for metrics.** Rejected:
  doubles sockets and TLS handshakes and contradicts `metrics-design.md` §5/§10
  (share the socket; HTTP + gRPC parity comes for free from sharing it).

The I/O thread remains the single consumer either way; this ICP only widens the
*producer* side, which the implementation already supports.

## Postscript — what the delay cost

Nothing raced. The implementation was mechanically MPSC-safe exactly as this
ICP argued: `m_pending_queue` under `m_pending_mu`, handle ids allocated with
an atomic `fetch_add`. What was missing was the *evidence*, and the contract
said the opposite of the code for four milestones.

The TSAN test this ICP asked for and that was never written earned its keep on
the first run — not by finding a transport bug, but by catching a
use-after-free in the test itself: `RequestSpec::payload` is a borrowed span
(`memory-model.md` §3.3), and the first draft let each payload buffer die at
the end of its loop iteration while `PayloadReadCb` was still reading it. That
is the contract this interface actually imposes on callers, and it took writing
a concurrent test to exercise it.

Found and resolved via the ICP index (`docs/icps/README.md`) and issue #134.
