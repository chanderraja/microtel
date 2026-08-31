# Interface Change Proposals (ICPs)

ICPs are short markdown documents that record breaking changes to interfaces locked in M0, or material changes to architecture documents that contributors and AI agents read as durable rules.

Per spec §13.2, an ICP is **lightweight, but visible**. A few paragraphs. They are heads-up documents, not multi-week reviews.

## Index

Status at a glance. **Append-only** (see File naming): nothing here is removed,
including superseded proposals — the rationale is the point, and an accepted
ICP is a decision still in force, not a spent one. Several are referenced
directly from `CLAUDE.md`, `CMakeLists.txt`, `ci/header_check.cpp`, and the
headers.

| ICP | Subject | Status | Implemented by |
|---|---|---|---|
| [0001](0001-m0-deliverables-clarification.md) | M0 deliverables clarification | Accepted | — |
| [0002](0002-vendor-tl-expected.md) | Vendor `tl::expected` for `microtel::Expected` | Accepted | — |
| [0003](0003-m0-deferred-decisions.md) | M0 deferred decisions | Accepted | — |
| [0004](0004-vendor-tomlplusplus.md) | Vendor toml++ | Accepted | — |
| [0005](0005-in-repo-benchmarks.md) | In-repo benchmarks | Accepted | — |
| [0006](0006-bench-drop-counter-naming.md) | Bench drop-counter naming | Accepted | — |
| [0007](0007-wire-codec-send-all.md) | `IWireCodec::SendAll` | Accepted | — |
| [0008](0008-metric-drop-reasons.md) | Metric `DropReason` values | Accepted | #75 |
| [0009](0009-transport-concurrent-send.md) | `ITransport::Send` safe for concurrent callers | Accepted (retroactively) | M12; docs + test in #156 |
| [0010](0010-milestone-renumber-views.md) | Milestone renumber (Views → M13) | Accepted | — |
| [0011](0011-log-attribute-limit-drop-reason.md) | `LogAttributeLimit` drop reason | Accepted | — |
| [0012](0012-provider-get-logger.md) | `Provider::GetLogger` | Accepted | — |
| [0013](0013-rescope-defer-python-bindings.md) | Defer Python bindings to M18 | Accepted | #107 |
| [0014](0014-otelcpp-shim-and-rule-13.md) | otel-cpp shim, source-only, rule 13 | Accepted | #107 |
| [0015](0015-unrepresentable-attribute-policy.md) | Unrepresentable attribute values | Accepted | #110 |
| [0016](0016-adapter-drop-accounting.md) | Adapter drop accounting (shim-local) | Accepted | #127 |
| [0017](0017-lazy-transport-connect.md) | Lazy transport connect | Accepted | #129 |
| [0018](0018-reconnect-after-drop.md) | Reconnect after a mid-connection drop | Accepted | #151, #153 |
| [0019](0019-connect-on-the-io-thread.md) | Perform `Connect` on the I/O thread | Draft | — |
| [0020](0020-install-and-package-config.md) | `install()` rules and the exported target set | Accepted | — (M9/M10) |
| [0021](0021-threading-model-reconciliation.md) | Reconcile `threading-model.md`; make LOCKED checkable | Accepted | — (scheduled) |

"Implemented by" is recorded only where a commit explicitly applies the ICP.
A dash means the link was not determinable from commit messages, **not** that
the ICP is unimplemented — several of the early ones were applied as part of
the milestone that motivated them. Grepping for the ICP number finds passing
mentions as often as implementations, so the column is deliberately sparse
rather than speculatively filled.

### ICP 0009 — resolved, and worth remembering

0009 proposed relaxing `ITransport::Send` from "single-caller" to "safe for
concurrent callers", so the M12 metrics pipeline could share one transport.
**The relaxation shipped and the ICP did not**: `SdkBuilder::Build` has built
three codecs over one transport since M12, each driven by its own exporter
worker, while `interfaces.md` §4.1 went on stating — LOCKED — that concurrent
`Send` was a contract violation. The TSAN test 0009 specified was never
written.

Nothing ever raced; the implementation was mechanically safe exactly as 0009
argued. But the contract asserted the opposite of the code for four
milestones, and the evidence for the safety claim did not exist until #156.

Kept here as the worked example of what issue #134 is about: the gap is not
that a document drifted, it is that nothing ever checked one against the other.

## When an ICP is required

After M0 closes, an ICP is required for:

- Breaking changes to any interface in `include/microtel/internal/*.hpp` or to the public API in `include/microtel/`.
- Changes to a contract documented in `docs/interfaces.md`.
- Changes to architectural commitments in `docs/architecture.md`, `docs/threading-model.md`, `docs/memory-model.md`, or `docs/error-model.md`.
- Changes to durable agent / contributor instructions in `CLAUDE.md`.
- Changes to the deliverable set or scope of a milestone in `microtel-spec.md` §13 or §14.

## When an ICP is *not* required

- Adding a new interface (no existing contract to break).
- Bug fixes that don't alter observable behavior.
- Documentation polish that doesn't change a normative claim.
- Test additions, formatting, comment-only changes.
- New methods on an interface that's still being drafted (M0 in progress).

## Pre-M0-close ICPs

Pre-close, the ICP process is *encouraged but optional*. It is a useful pattern for changes that materially reshape the M0 deliverable set or amend CLAUDE.md / spec / repository-layout.md in ways that future contributors should be able to find. ICP 0001 is an example.

## File naming

`NNNN-short-slug.md`, four-digit zero-padded, monotonically increasing. Append-only — superseded ICPs stay; a new ICP records the supersession.

## Process

1. PR an ICP into `docs/icps/`.
2. Reviewer sign-off (single reviewer pre-1.0).
3. Merge the ICP **before** the implementing PR, so the implementing PR can reference it by number.
4. Implementing PR makes the substantive changes.

## Required sections

Every ICP includes:

- **Status:** Draft | Accepted | Implemented | Superseded.
- **Affected interfaces / docs:** explicit file list.
- **Affected tracks:** which of Tracks A–F (spec §13.1) consume what's changing — or "none / docs only".
- **Summary:** one sentence.
- **Motivation:** why now.
- **Proposed change:** what changes, with file paths.
- **Migration:** what contributors / agents need to do differently.
- **Rationale & alternatives:** what other shapes were considered.
