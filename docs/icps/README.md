# Interface Change Proposals (ICPs)

ICPs are short markdown documents that record breaking changes to interfaces locked in M0, or material changes to architecture documents that contributors and AI agents read as durable rules.

Per spec §13.2, an ICP is **lightweight, but visible**. A few paragraphs. They are heads-up documents, not multi-week reviews.

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
