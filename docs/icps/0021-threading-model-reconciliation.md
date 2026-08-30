# ICP 0021: Reconcile `threading-model.md` with the code, and stop LOCKED drifting again

**Status:** Draft
**Affected interfaces / docs:** [`docs/threading-model.md`](../threading-model.md)
§2, §2.2, §3.3, §4, §5.1, §5.3 — all **LOCKED**, which is why this is an ICP
rather than a documentation PR. Proposes a mechanism change affecting
`docs/icps/README.md` and CI.
**Affected tracks:** all — this document is what every track reads for
threading rules.

## Summary

Six LOCKED claims in `threading-model.md` describe a system that does not
exist. This ICP corrects them, retroactively, in the same shape as ICP 0009 —
the code shipped something the locked document forbade, and the document lost.
It then proposes the cheapest mechanism that would have caught any of them.

No source changes. Every correction makes the document match code that already
works.

## Why an ICP and not a docs PR

`docs/icps/README.md` requires an ICP for *"changes to architectural
commitments in … `docs/threading-model.md`"*. Two of the corrections below —
§2's thread-role count and §2.2's worker count — are not merely stale prose;
they are **design constraints the implementation violated**, and accepting
reality is a change of commitment. The rest are factual corrections that ride
along rather than being split across two processes.

## The six claims

### 1. §2 — "v1 has exactly three thread roles" (LOCKED)

**Seven** threads exist, in seven classes:

| Class | Member |
|---|---|
| `BatchSpanProcessor` | `m_worker` |
| `BatchLogRecordProcessor` | `m_worker` |
| `OtlpExporter` | `m_worker` |
| `OtlpMetricExporter` | `m_worker` |
| `OtlpLogExporter` | `m_worker` |
| `PeriodicExportingMetricReader` | `m_thread` |
| `Http2Transport` | `m_io_thread` |

The *taxonomy* is also wrong, not only the count: §2.2 describes one "exporter
worker" that drains the span queue **and** encodes **and** sends. Those are two
different threads — the processor worker hands off to the exporter worker,
which encodes and calls the codec.

**Correct to:** three thread *roles* (producer / processor-worker /
exporter-worker) plus the I/O thread, with seven instances, and an explicit
inventory.

### 2. §2.2 — "v1 always has exactly one worker per process" (LOCKED)

Five threads match the functional description ("drain a queue, encode, send"):
the three exporters and the two batch processors. The parenthetical
justification — *"Multi-profile (multiple `Provider` instances per process) is
a v1.1 feature"* — is a non-sequitur: the multiplicity comes from **three
signals inside one `Provider`**, not from multiple providers.

**Correct to:** one worker *per pipeline*, of which there are up to five in a
fully-configured single `Provider`.

### 3. §5.3 — `std::atomic<ShutdownState> m_state` (LOCKED, "single-source-of-truth")

`ShutdownState` **has never existed in any commit** (`git log -S` is empty).
`SdkProvider` has no such member. Shutdown state is spread across nine
independent flags in seven objects, and ordering is achieved by member
declaration order.

This is the marker that prompted issue #134.

**Correct to:** describe the actual mechanism — per-component shutdown flags
plus declaration-order teardown — and drop the "single-source-of-truth"
claim, which was never true. Note that `SdkProvider::m_shut_down` (added in
#145, used by the fork handler in #148) is the closest thing to the intended
design and covers only the entry points that consult it.

### 4. §4 — the lock table names three mutexes that do not exist

| Table name | Reality |
|---|---|
| `m_diag` | `DiagnosticsCounters::m_error_mu` |
| `m_queue` | `BatchSpanProcessor::m_mu` |
| `m_transport_request` | `Http2Transport::m_pending_mu` |
| `m_completion` | **Does not exist** — completion uses `std::promise`/`std::future`, not a mutex+condvar |
| `m_shutdown` | **Not a mutex** — `OtlpExporter::m_shutdown` is `std::atomic<bool>`; `SdkProvider` has no such member at all |

Rules 3 and 4 are stated entirely over `m_completion` and `m_shutdown`, so they
are unfalsifiable as written. Nine real locks are missing from the table
(`m_meter_mu`, `m_logger_mu`, `EpollReactor::m_mu`, the metric-reader pair, the
four metric-storage locks, `m_cancel_mu`).

**Correct to:** the real inventory, with rules restated over locks that exist.
Rule 1 (`m_diag` is a leaf) is **verified** and stands.

### 5. §5.1 — "two file descriptors besides the socket"

Named `m_request_eventfd` and `m_shutdown_eventfd`. There is **one**:
`EpollReactor::m_wake_fd`, shared by both wake reasons via `Wake()`.

### 6. §3.3 — "waits on a `std::condition_variable` keyed to the in-flight request"

It is a `std::promise` / `std::future` pair. Functionally equivalent, but §4's
lock table treats `m_completion` as a real lock on the strength of this
sentence.

## Proposed mechanism — make LOCKED checkable

Correcting six claims fixes today. It does not stop the seventh. The audit
found roughly 45% of LOCKED markers not describing the system, and **the
mechanism intended to prevent drift never had a verification step**: every
marker was applied in the M0 commit, before any code existed to check against,
and nothing re-checks.

Proposed, in increasing cost:

1. **A LOCKED claim about code must cite the code.** Any marker asserting
   something checkable carries a `file:function` reference — function, not line
   number, because line numbers rot (a citation added in #149 was already stale
   by #144). Claims about intent or policy carry no citation and are explicitly
   exempt.
2. **CI checks the citations resolve.** A script greps each cited symbol and
   fails if it is absent. This is cheap and would have caught the two worst
   findings in this project's history — `ShutdownState` and `UpbArena`, both
   types named in multiple documents that no commit ever contained.
3. **An audit obligation at milestone close** — re-verify the markers in
   documents the milestone touched.

**(1) and (2) are proposed for adoption; (3) is noted and not proposed**, since
a process obligation nobody is scheduled to perform is how this situation arose
in the first place.

(2) is deliberately weak: it proves a *symbol exists*, not that the surrounding
claim is true. It would not have caught §2's thread count. It is proposed
because it is nearly free and catches the specific failure mode — a named type
that never existed — that produced the two most expensive findings here.

## Migration

- Documentation only; no source change.
- `docs/icps/README.md` gains the citation rule.
- CI gains a citation-check script if (2) is adopted.
- Contributors: a new LOCKED claim about code needs a `file:function` citation.

## Open questions for the reviewer

1. Adopt (1) and (2), or (1) alone? (2) is a CI job that will occasionally fail
   for renames, which is the point, but it is a cost.
2. §2's "three thread roles" — correct to seven instances of four roles as
   proposed, or is the role taxonomy itself worth redesigning now that logs and
   metrics have their own pipelines?
3. Should this ICP also correct `error-model.md` and `memory-model.md`, which
   the same audit found similarly divergent? Proposed: no — one document per
   ICP keeps the diff reviewable, and `threading-model.md` is the one whose
   claims other tracks actually rely on.
