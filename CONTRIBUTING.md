# Contributing to microtel

Thanks for your interest in contributing.

This document is intentionally short — the heavy details live in other documents that you'll be pointed at as needed. Read this once; consult [CLAUDE.md](CLAUDE.md) and [docs/coding-standards.md](docs/coding-standards.md) every time you write code.

---

## Project status

microtel is **pre-1.0** and currently in the architecture and design phase (milestone M0).

**External implementation PRs are not being accepted until v1.0 ships.** This is deliberate and not personal. The internal architecture is still being designed; accepting outside implementation contributions during design slows everything down for everyone, including the contributors themselves (work that's based on architecture-in-flux gets reworked or thrown away). The bar for accepting an implementation PR lifts when v1.0 ships and the architecture is stable.

What's welcome right now:

| Welcome | Not yet |
|---|---|
| Issues for spec ambiguities or contradictions | Implementation PRs |
| Architecture discussion in Issues / Discussions | New feature PRs |
| Reviewing interface designs as they're proposed | Refactor PRs against code that doesn't exist yet |
| Test scenarios for the planned protocols | Forks for vendor-specific exporters |
| Typo fixes and trivial doc improvements (PRs OK) | Style / formatting PRs that don't change correctness |
| Comparison data with `opentelemetry-cpp` for benchmarks | New dependencies |

If you want to track when contribution opens up, watch the repo or follow the [roadmap](microtel-roadmap.md). M2 (Skeleton & contracts) is when *internal* parallel implementation tracks unblock; **external** PRs become accepted at v1.0 release (M10).

If you have a feature you want in v1.0 that isn't already on the roadmap, file an Issue with the feature-request template — well-argued requests can shape what v1.0 includes.

---

## Before you start

1. **Read [CLAUDE.md](CLAUDE.md).** It's titled for AI coding agents but it's the durable rules document for everyone — humans and agents alike. The hard rules are the same regardless of who's typing.
2. **Read the relevant spec section.** [microtel-spec.md](microtel-spec.md) is the contract. If you're touching trace SDK code, read §5–§6. If you're touching wire codecs, read §7. If you're touching config, read §12.
3. **Read [docs/coding-standards.md](docs/coding-standards.md).** This is the SonarQube-aligned ruleset CI enforces.

---

## Reporting bugs

Use GitHub Issues with the **bug report template**. Include:

- microtel version (or commit SHA if pre-release).
- Build configuration (compiler, build type, sanitizers if any).
- Platform.
- Minimal reproduction.
- What you expected vs what happened.

For **security vulnerabilities**, see [SECURITY.md](SECURITY.md). Don't open public issues for security issues.

## Suggesting features

Use GitHub Issues with the **feature request template**. Describe the use case before the proposed solution. If your suggestion would change a public interface, the response will be to open an [Interface Change Proposal](docs/icps/) — this is a lightweight markdown document, not a heavyweight process.

## Submitting code changes

**Pre-1.0:** external implementation PRs are not currently accepted. See the "Project status" section above. This section describes the process that applies once v1.0 ships, and is also the process for the small number of pre-1.0 PRs that are accepted (typo fixes, doc improvements).

The project uses pull requests on GitHub.

1. Fork the repo and create a feature branch.
2. Follow the [coding standards](docs/coding-standards.md) — CI will reject PRs that don't.
3. **Tests first.** TDD compliance is mechanically enforced (per spec §14.2):
   - **Diff coverage gate:** every changed source line in your PR must be covered by a test in the same PR. Threshold: 90% on SDK and encoder; 80% on transport and exporter.
   - **Test-presence gate:** any change to `src/**/*.{cpp,hpp}` requires a corresponding change to `tests/**/*.{cpp,hpp}` unless your PR has the `[refactor]` label.
4. **RAII discipline.** No raw `new`/`delete`. Every resource is owned by an RAII type.
5. Open a PR using the template. Fill in the review checklist.
6. CI will run the full gauntlet — build matrix, sanitizers, static analysis, coverage, fuzz smoke. Address any failures.
7. A maintainer reviews and either merges or requests changes.

### Commit messages

- Short summary line (≤ 72 characters).
- Optional longer body explaining *why*, not *what*.
- Reference the issue if applicable: `Fixes #42` or `Refs #42`.

### Interface changes

Once an interface is locked in `docs/interfaces.md` (post-M0 sign-off), changing it requires an [Interface Change Proposal](docs/icps/). ICPs are short markdown documents — typically a few paragraphs identifying:

- The interface and the change you propose.
- Affected components and tracks.
- Migration path for existing call sites.

ICPs are reviewed and either accepted (merged into `docs/icps/`) or rejected with feedback. Lightweight and visible.

---

## What gets reviewed

Reviewers focus on:

- **API design** — clarity, consistency with the rest of the project, correctness of error model.
- **RAII compliance** — no raw `new`/`delete`, ownership clear, move-only where appropriate.
- **Test coverage** — tests exist (CI gate), and they're meaningful (judgment call).
- **Complexity** — function and file sizes within limits, no clever-but-unmaintainable constructs.
- **Threading and lifetime** — clearly stated for any new type.
- **Security implications** — for changes touching crypto, parsing, or network handling.

Reviewers do *not* spend time on style — that's `clang-format`'s job. PRs that haven't been auto-formatted fail CI before reaching review.

---

## Code of conduct

A formal code of conduct will be adopted before the first public release. In the meantime: be respectful, be constructive, and assume good faith.

## License and DCO

By contributing, you agree that your contributions are licensed under the project's [Apache 2.0 license](LICENSE).

The decision between **DCO sign-off** (`Signed-off-by:` line in commit messages) and a **CLA** is TBD before the first public release. Pre-1.0 contributors will be asked to confirm consent under the chosen mechanism once finalized. See [microtel-spec.md §19 Governance](microtel-spec.md).

---

## Local knowledge graph

The project ships with a [graphify](https://github.com/safishamsi/graphifyy) knowledge graph that maps every source file, internal interface, and design concept into a queryable graph with community detection. Agents (Claude Code) use it as their primary navigation tool — it reduces per-query token cost by ~7× vs. reading raw files.

The graph is **not committed to git** (it's large and auto-generated). To bootstrap on a fresh clone:

1. Install the `graphifyy` package into a Python virtual environment (the project convention is `~/.venv`):
   ```bash
   python3 -m venv ~/.venv
   ~/.venv/bin/pip install graphifyy
   ```
2. In Claude Code, run `/graphify` from the repo root. This runs the full pipeline (~2 min on first run) and installs git hooks that keep the graph current on subsequent commits — no LLM cost for incremental updates.

After bootstrap, `graphify-out/` appears in the repo root and is gitignored. Claude Code reads `graphify-out/wiki/index.md` as its codebase map in every session.

A static snapshot of the community report is committed at [docs/graph-report.md](docs/graph-report.md) for reference without running the pipeline.

---

## Questions

For project questions that don't fit an issue, use GitHub Discussions (or, pre-public-release, contact the maintainer directly).
