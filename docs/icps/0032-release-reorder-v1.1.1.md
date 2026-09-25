# ICP 0032: add a v1.1.1 patch release and swap the v1.2 and v1.3 themes

**Status:** Draft.
**Affected interfaces / docs:**
- `microtel-spec.md` §18.2, §18.3 and §18.4
- `microtel-roadmap.md`:
  - status table
  - §3 tier table
  - §4 (v1.2 and v1.3 sections)
  - §5, §6, §7, §8, §9
  - §11: one new decision-log row
- Every other document that names v1.2 or v1.3 for a specific piece of work,
  about 40 references. The largest are:
  - `README.md`
  - `examples/README.md`
  - `docs/interop-matrix.md`
  - `docs/control-plane-design.md`
  - `tests/conformance/README.md`

No interface or header changes.

**Affected tracks:** documentation and release planning only.

## Summary

Ship a v1.1.1 patch release with the two fixes the leaf / concentrator work
depends on. Then swap the order of the next two minor releases so that
v1.2 is logs plus the leaf / concentrator, and v1.3 is metrics plus the
control plane.

## Motivation

ICP 0031 moved the leaf / concentrator into the logs release. That left it
behind a metrics release whose remaining work mostly doesn't touch the leaf:
- metric feature gaps (#237, #298–#301)
- metrics conformance (#302)
- the control-plane socket (#296)
- mTLS rotation (#297)

Only two open issues block the concentrator:

- **#257:** table-valued settings are replaced wholesale instead of merged per
  key. The concentrator's core job is merging per-leaf Resource attributes into
  its own. That can't be specified or built while a higher-precedence table
  wipes out a lower one.
- **#222:** metric and log exporters have no retry. A leaf never retries, so
  the concentrator's exporter is the only thing that stands between a leaf's
  data and loss. Logs going supported needs this anyway.

## Decisions

### 1. v1.1.1

v1.1.1 carries #257, #222 and #284 (log the resolved Resource at startup),
which anyone debugging per-leaf enrichment will need. The GitHub milestone is
`v1.1.1`.

Because this is a patch release, it adds **no public API**:

- **#222** reuses the existing `TimeoutOptions::retry_budget` and the trace
  exporter's retry engine.
- **#257** changes behaviour: a key set in a higher-precedence source overrides
  that key, and no longer discards the rest of the table. It is released as a
  bug fix because spec §12.7 describes precedence between sources, not
  replacement of whole tables. The release notes call it out as a behaviour
  change.

### 2. v1.2 and v1.3 swap

| Release | Before | After |
|---|---|---|
| v1.2 | Metrics + control plane | **Logs + leaf / concentrator** (ICP 0031's content, unchanged) |
| v1.3 | Logs (+ leaf / concentrator, ICP 0031) | **Metrics + control plane** |

The themes move whole. Nothing is added to or removed from either one, and
ICP 0031's decisions and ship gates apply as written, with "v1.3" read as
"v1.2".

What changes as a result:

- **Metrics** stay experimental one release longer.
- **The control-plane socket** (ICP 0024) moves from v1.2 to v1.3.
- **General bugs carried over from v1.1** go to v1.2, so they are not pushed
  two releases out:
  - #223, #238, #267, #271, #274, #282, #285
  - #134, #277
- **Metrics-specific bugs** stay with metrics in v1.3: #237, #259.

### 3. Records are not rewritten

ICP 0024 ("socket in v1.2") and ICP 0031 (filename `…-in-v1.3.md`) stay as
they are, because ICPs are append-only. This ICP is the record of the
renumbering. Living documents (spec, roadmap, READMEs, design docs) are
updated to the new numbers.

## Migration

After acceptance, one docs PR makes these changes:

- **PR #308:** folded in. It applies ICP 0031 and currently says v1.3; it is
  updated to say v1.2.
- **References to the two releases:** every reference outside `docs/icps/` is
  renumbered. That covers v1.2 for metrics or the control plane, and v1.3 for
  logs or the leaf.
- **Roadmap decision log:** gains one row for this reordering.

The GitHub milestones are already renamed to match.

## Rationale & alternatives

- **Keep the order and start the leaf in parallel.** Work could overlap, but
  the leaf would still ship after the whole metrics release. That is the delay
  this ICP exists to remove.
- **Put #257 and #222 in v1.2 instead of a patch.** That is simpler, but it
  holds two fixes (one of them a data-loss gap) until a feature release. It
  also means leaf implementation can't start against a released baseline.
- **Pull only the leaf forward, as its own release.** This would split logs
  from the leaf again and add a third release to plan. Logs are small, and
  ICP 0031 already made the two halves independent.
