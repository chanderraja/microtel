# ICP 0010: Reconcile spec §13 milestone table with as-built commit numbering (insert M13 = Views)

**Status:** Draft
**Affected interfaces / docs:** `microtel-spec.md` §13 (Roadmap milestone table only). No interface headers, no `docs/interfaces.md`.
**Affected tracks:** none — documentation reconciliation.

## Summary

Insert a **M13 – Views & attribute filtering** milestone into the spec §13
table and renumber Logs → M14, control plane → M15, sugar → M16, so the spec
matches the milestone labels already used in merged commit history.

## Motivation

Commit milestone labels track spec §13 exactly through **M12**, then drift by
one. An unplanned **Views** milestone was implemented and tracked as **M13**
(increments 1–4, PRs #85–#88), but the spec table has no Views row — Views was
scoped inside M12 and specified in the M11 design doc §6. That insertion
shifted the tail: the spec's **M13 (Logs)** is being built under the commit
label **M14** (`M14 L1/L2/L3`, PRs #89–#91), and the spec's M14/M15 will land
as commit M15/M16.

The drifting labels are already in merged, branch-protected history (Views and
Logs-L1 are on `origin/master`), so they cannot be renumbered without a
force-push against branch protection. Left unreconciled, the "spec M13 = Logs
but commits M14 = Logs" mismatch imposes a cognitive tax that compounds with
every future milestone.

## Proposed change

Adopt the as-built numbering in `microtel-spec.md` §13. M0–M12 unchanged.

| Spec today | Becomes | Depends on | Notes |
|---|---|---|---|
| — | **M13 – Views & attribute filtering** (NEW) | M12 | Per-view aggregation selection via `ViewRegistry` matching + `attribute_allowlist` for synchronous and observable instruments. |
| M13 – Logs | **M14 – Logs** | M12 | Scope unchanged; still depends on M12 (Logs is independent of Views). |
| M14 – Control plane + hot reload | **M15 – Control plane + hot reload** | M10 | Scope unchanged; "parallel with" updated to M12, M13, M14. |
| M15 – Sugar layer | **M16 – Sugar layer** | M10 | Scope unchanged. |

A one-line numbering note is added under the table pointing back to this ICP.
The substantive edits land in the same PR as this ICP (docs-only, no
implementing PR to follow).

## Migration

- Contributors and agents: when a commit says "M14," it implements **Logs**
  (spec M14 after this change); "M13" implements **Views**. Future
  control-plane work is **M15**, sugar is **M16**.
- No code, interface, or test changes. Nothing to rebuild.

## Rationale & alternatives

- **Renumber commits to match the spec** — rejected: merged, branch-protected
  history; would require a force-push.
- **Keep spec numbers canonical, add only a numbering note** — less disruptive
  (no M14→M15 / M15→M16 shift) but leaves spec and commit labels permanently
  one apart; the confusion grows with each milestone. Rejected in favor of a
  one-time correction.
- **Fold Views back into M12 in the spec** — misrepresents reality: Views
  shipped as four increments across four PRs with its own `ViewRegistry`, large
  enough to be a milestone rather than a footnote.

## Secondary observation (not part of this renumber)

Spec **M8 (Python bindings, traces-only)** has no implementation — history goes
M7 → M9. This is consistent with §13's note that M8 is "not a release blocker"
and may ship as v1.0.1, so no renumber is warranted. Flagged for the record; a
future ICP can restate M8's target version if desired.
