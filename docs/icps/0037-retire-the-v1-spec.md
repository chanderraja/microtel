# ICP 0037: retire `microtel-spec.md` as the source of truth

**Status:** Draft — needs the maintainer's sign-off.
**Affected interfaces / docs:**
- `microtel-spec.md` (frozen: a banner, no other edits)
- `docs/compatibility-matrix.md` (gains the compatibility tiers, spec §2.2)
- `RELEASING.md` (gains the versioning and ABI policy, spec §19)
- `docs/bench-spec.md` (gains the performance and footprint targets, spec §10)
- `CLAUDE.md` ("Authoritative documents"), `CONTRIBUTING.md` ("Read the
  relevant spec section"), `docs/architecture.md` (its "Source of truth for
  rationale" line), `docs/README.md` (the note on the spec)

**Affected tracks:** documentation only. No code, no public or internal
interface.

## Summary

`microtel-spec.md` stops being the project's source of truth. The three
parts of it that still define rules nothing else defines move to the live
documents readers already use. The file stays at its path, frozen, as the
historical v1.0 design specification, so the citations of the form "spec §N"
in about 120 files (docs, ICPs, tests and source) still resolve.

## Motivation

The spec was written in M0 as the contract for the v1.0 release and has not
been re-baselined since. Everything after v1.0 was specified elsewhere: the
metrics, logs and leaf design docs, the ICPs, `docs/interfaces.md`,
`docs/configuration.md`. ICPs patched the spec with short annotations
instead of rewriting it. The result reads as wrong to anyone who starts
there: §1 says "v1 is exporter-first with traces as the only signal", §8's
feature table marks metrics, logs and the sugar layer ❌, and §13 carries a
warning that "the implementation has run ahead of" it. Yet `CLAUDE.md`
calls it "Source of truth for everything in v1.0" and `CONTRIBUTING.md`
calls it "the contract". Reviews this week kept tripping over that gap.

Re-baselining it as a living spec would duplicate the design docs and drift
again. Deleting it would break the citations and lose the original
rationale (§1–§5), which is still worth reading.

## Proposed change

1. **Move the three parts still in force, and only those.** Each move copies
   the text, adapted only where it names a release as current:

   | Spec | What it defines | New home |
   |---|---|---|
   | §2.2 | The compatibility tiers (Tier 1 OTLP wire, Tier 2 data model, Tier 3 API adapter, Tier 4 full SDK conformance), which the roadmap, the migration guide, `metrics-design.md`, `ci-architecture.md` and `control-plane-design.md` all use | a new last section of `docs/compatibility-matrix.md` (last, so §4, which many docs cite, keeps its number) |
   | §19 | Semantic versioning, the ABI policy (source-compatible within a major; binary ABI best-effort within a minor, not guaranteed across minors), release cadence | a new section of `RELEASING.md` |
   | §10 | The hot-path, exporter, footprint and cold-start metrics, and the v1 footprint targets | a new section of `docs/bench-spec.md` |

   Everything else already has a maintained home and needs no move. The
   mapping goes into `docs/README.md` so a reader of a "spec §N" citation can
   find where that topic lives now:

   | Spec | Now lives in |
   |---|---|
   | §1–§4 Summary, goals, non-goals, motivation | README "Why it exists"; roadmap anti-goals (historical rationale stays in the spec) |
   | §5 Architecture | `docs/architecture.md`, threading, memory and error models |
   | §6 API, §8 SDK features | the public headers' Doxygen, `docs/interfaces.md`, README Status |
   | §7 Wire protocols | `docs/grpc-wire-protocol.md` |
   | §9 Build & dependencies | `docs/build-options.md`; CLAUDE.md rules 12–13 |
   | §11 Project structure | `docs/repository-layout.md` |
   | §12 Configuration | `docs/configuration.md` |
   | §13, §16–§18 Roadmap, risks, open questions, future | `microtel-roadmap.md` and the design docs (§13's milestone history stays in the spec) |
   | §14 Engineering practices | `CONTRIBUTING.md`, `docs/coding-standards.md`, `docs/ci-architecture.md` |
   | §15 Compatibility & interop | `docs/compatibility-matrix.md`, `docs/interop-matrix.md` |
   | §19 DCO, security, CODEOWNERS, CI quality gates | `CONTRIBUTING.md`, `SECURITY.md`, `CODEOWNERS`, `docs/ci-architecture.md` |

2. **Freeze the spec.** A banner at the top: this is the v1.0 design
   specification as of v1.0, kept for its rationale and for the citations
   into it; it is not maintained; current behaviour is defined by the
   documents listed in `docs/README.md` and by the ICPs. No other edit.

3. **Repoint the authority.** `CLAUDE.md` "Authoritative documents" and
   `CONTRIBUTING.md` step 2 name the live documents (the design docs,
   `docs/interfaces.md`, `docs/configuration.md`, the ICPs, the roadmap)
   instead of the spec. `docs/architecture.md` drops "Source of truth for
   rationale: spec §5" in favour of "original rationale: spec §5 (historical)".

4. **Future ICPs amend the live document,** never the spec.

## Migration

None for code. Existing "spec §N" citations stay valid and need no edit:
they now point at a historical record, which is what most of them (milestone
history, original rationale) already were. Citations of §2.2, §10 and §19
may be updated to the new homes opportunistically.

## Rationale & alternatives

- **Re-baseline the spec as a living v1.x document.** Rejected: a large
  rewrite that would duplicate the design docs and drift again; the design
  docs plus ICPs are already the working record.
- **Delete it.** Rejected: citations in about 120 files would break, and the original
  design rationale would be lost.
- **Banner only, move nothing.** Rejected: the tiers and the ABI policy would
  have no current home, so the banner would point readers at nothing for the
  two rules they most often need from it.
- **Defer to v2.0.** The roadmap treats v2.0 as a re-cut, but the confusion
  is costing review time now and the change is documentation only.
