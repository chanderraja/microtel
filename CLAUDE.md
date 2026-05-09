# microtel — Agent Instructions

Durable rules-of-engagement for Claude Code (and any other AI coding agent) working on this project. Read this once per session before doing anything else.

---

## Project at a glance

`microtel` is a lightweight OpenTelemetry-compatible **trace runtime and OTLP exporter** built on nghttp2. It speaks both **OTLP/HTTP-protobuf** and **OTLP/gRPC** on the wire without linking the gRPC library — the gRPC path is implemented as a small unary-RPC protocol layer over the same nghttp2 transport.

**v1 is exporter-first, traces only.** Metrics, logs, control plane, hot reload, sugar APIs, and broader SDK coverage are deferred to v1.1 and beyond.

### Authoritative documents

- **`microtel-spec.md`** — the v1 specification. Source of truth for everything in v1.0.
- **`microtel-roadmap.md`** — multi-year roadmap, v1.0 → v3.0. Source of truth for what's deferred and when.
- **`docs/interfaces.md`** — internal interface contracts. Created in M0; sign-off required before M1 begins.

---

## Current phase: check before you act

1. If `docs/interfaces.md` doesn't exist or isn't signed off → you're in **M0 (Architecture & Design)**. No source code yet, only design docs and compilable interface header stubs.
2. If `docs/interfaces.md` is signed off but no `src/` exists → you're in **M1 (Spike)**. Throwaway code that validates M0's architecture decisions.
3. If `src/` exists with mocks under `tests/mocks/` → you're in **M2 (Skeleton & contracts)** or later. TDD applies from M3 onward.

When in doubt, run `git log --oneline -20` and look at recent commits for clues.

---

## Hard rules

These are non-negotiable. Violations produce PRs that fail CI or get rejected in review.

### Phase discipline

1. **No source code in M0.** Only documents in `docs/` and compilable header stubs in `include/microtel/` (public API) and `include/microtel/internal/` (internal interfaces). Public headers carry full Doxygen and method signatures with no bodies; internal interface headers are pure-virtual / abstract with full Doxygen. Both compile under an `INTERFACE` CMake target with `-Werror`. No method implementations. Implementation begins in M3. Captured in [`docs/icps/0001-m0-deliverables-clarification.md`](docs/icps/0001-m0-deliverables-clarification.md).
2. **No interface changes after M0 sign-off without an ICP.** Breaking changes to any locked interface require a markdown ICP PR'd into `docs/icps/` identifying affected components and migration path.

### TDD enforcement (M3 onward)

3. **Tests first, every time.** Write the failing test, then the minimal code to pass it, then refactor. CI gates enforce this mechanically:
   - **Diff coverage:** every line added/modified in a PR must be exercised by a test in the same PR. Threshold: 90% on SDK and encoder; 80% on transport and exporter.
   - **Test-presence:** any change to `src/**/*.{cpp,hpp}` requires a corresponding change to `tests/**/*.{cpp,hpp}`. Documented exceptions: pure refactors with the `[refactor]` PR label, formatting/comment-only changes, code deletions.
4. **Mocks are dumb.** They return what they're configured to return. No logic. If you need logic, write a fake under `tests/fakes/`. "Smart mocks" are a code smell.

### RAII discipline (always)

5. **Every resource is RAII-owned.** Heap memory, file descriptors, sockets, mutexes, threads, OpenSSL contexts, nghttp2 sessions, upb arenas, callback registrations — every one of them lives inside a type whose destructor releases it.
6. **No raw `new`/`delete`** in production code. Use `std::make_unique` / `std::make_shared`.
7. **Raw pointers are non-owning by definition.** A function taking `T*` does not delete; a function returning `T*` returns a borrowed reference whose lifetime is documented in Doxygen.
8. **`std::shared_ptr` requires justification in code review.** Default to `std::unique_ptr`. Use `std::weak_ptr` for breaking cycles.
9. **Custom RAII wrappers** for C resources live in `src/common/raii/`: `Socket`, `SslCtx`, `SslSession`, `Nghttp2Session`, `UpbArena`. Each is move-only with a `noexcept` destructor and a `release()` method.
10. **Resource-owning types are move-only by default.** Add copy semantics only with explicit justification.
11. **Rule of zero or rule of five.** Never the compiler-generated mix.

### SonarQube-aligned static analysis (always)

CI runs clang-tidy with a SonarQube-aligned ruleset; full list in `docs/coding-standards.md`. The most-likely-to-trip rules:

- **No `goto`.**
- **No commented-out code** in commits.
- **Cognitive complexity ≤ 15** per function.
- **Cyclomatic complexity ≤ 10** per function.
- **No magic numbers** — use `constexpr` named constants.
- **No naked `catch(...)`.**
- **No unsafe C functions:** `strcpy`, `strcat`, `sprintf`, `gets`, unbounded `scanf`. Use `snprintf` or C++ alternatives.
- **All non-static class members initialized** at declaration or in every constructor.
- **Const correctness:** mark methods, parameters, locals `const` where applicable.
- **Max 3 indentation levels** per function (refactor at 4+).
- **No nested ternaries.**
- **Max 7 parameters** per function (use a struct beyond that).
- **No empty `if`/`else`/`catch` blocks.**
- **All non-trivial public APIs** documented in Doxygen.
- **No unnamed namespaces in headers.**
- **No `using namespace`** at file scope in headers.
- **Header include order:** own header, project headers, system headers (LLVM convention).

### Dependency discipline

12. **The runtime dependency closure for v1 is fixed:** nghttp2, OpenSSL, upb (vendored), zlib, plus optional spdlog. **No new runtime dependencies without an ICP.** This is the project's whole reason to exist.
13. **No gRPC library, no abseil, no protobuf-cpp runtime.** Ever. The wire encoder is upb. The HTTP/2 transport is nghttp2 directly.

### Public API contracts

14. **Hot-path methods are `noexcept`.** `StartSpan`, `SetAttribute`, `AddEvent`, `End` never throw — on failure they drop the record/field, increment diagnostics, and return.
15. **Destructors are `noexcept`** and must not block indefinitely. They invoke `Shutdown` with a small finite timeout if not already shut down.
16. **Initialization returns `microtel::Expected<T, Error>`**, not exceptions. `microtel::Expected` is a project-local alias defined in [`include/microtel/expected.hpp`](include/microtel/expected.hpp); it resolves to `std::expected` on C++23 and to a vendored `tl::expected` on C++20. See [`docs/icps/0002-vendor-tl-expected.md`](docs/icps/0002-vendor-tl-expected.md).
17. **`ForceFlush(timeout)` and `Shutdown(timeout)` return a structured status** (`Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`).

---

## Style cheatsheet

- Allman braces.
- `m_` prefix on members.
- PascalCase types; snake_case where OTel naming dictates on the public API.
- C++20: prefer `std::span`, `microtel::Expected` (alias — see ICP 0002), designated initializers, concepts for template constraints.
- Doxygen comments on every non-trivial public API.

---

## Build & test

(Commands locked in by M2; this section is a placeholder until then.)

```bash
cmake --preset Release
cmake --build build/Release
ctest --test-dir build/Release
```

CI configuration lives in `ci/`. PRs run: build matrix (Linux x86_64, ARM64), all test categories (unit / integration / conformance / wire / fuzz), sanitizers (asan / tsan / ubsan), clang-tidy, clang-format, aggregate coverage, diff coverage, test-presence check, SonarQube Cloud (OSS tier).

---

## File ownership (post-M2)

After M2 lands the project skeleton, each `src/<directory>/` is owned per `CODEOWNERS`. Each directory carries a one-screen `README.md` describing:

- What lives there.
- Which interfaces it implements.
- What it depends on (and where the mocks are).
- Test entry points.
- Style notes specific to the directory.

**Read the directory README before editing files in it.** This is your local map.

---

## What to do FIRST in any session

1. Run `git status` and `git log --oneline -20`.
2. Read this file (CLAUDE.md).
3. Determine the current phase (see "Current phase" above).
4. Read the spec section relevant to the phase you're in.
5. If working in `src/`, read that directory's `README.md`.
6. If touching an interface, read its entry in `docs/interfaces.md`.

---

## What NOT to do

- **Don't add source code in M0** beyond compilable interface stubs.
- **Don't add a runtime dependency** without an ICP.
- **Don't break the build** intending to fix it later.
- **Don't merge without CI green.**
- **Don't bypass the test-presence check** by mislabeling a real change as `[refactor]`.
- **Don't write multi-paragraph commit messages** — short summary, longer body if needed, reference the issue.
- **Don't use `goto`, raw `new`/`delete`, `using namespace` in headers**, or any banned C function.
- **Don't introduce `std::shared_ptr` without justification** in the PR description.
- **Don't write "smart mocks"** — mocks are dumb, fakes have logic.
- **Don't expose upb symbols** outside `src/wire/encoder/`. The `OtlpEncoder` C++ wrapper is the only file that touches upb directly.

---

## Working with the user

- **Iterate.** Propose, get feedback, revise. Don't try to ship multi-component changes in one shot.
- **Ask clarifying questions** when the spec is ambiguous, before writing code.
- **Surface decisions, don't make them silently.** If a change requires picking between two reasonable approaches, ask which the user prefers.
- **Use the ICP process** for interface changes — don't argue them in PR comments.
- **Keep ICPs short.** A few paragraphs. They're heads-up documents, not multi-week reviews.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- ALWAYS read graphify-out/GRAPH_REPORT.md before reading any source files, running grep/glob searches, or answering codebase questions. The graph is your primary map of the codebase.
- IF graphify-out/wiki/index.md EXISTS, navigate it instead of reading raw files
- For cross-module "how does X relate to Y" questions, prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, or `graphify explain "<concept>"` over grep — these traverse the graph's EXTRACTED + INFERRED edges instead of scanning files
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- After merging a PR, refresh the committed snapshot: `cp graphify-out/GRAPH_REPORT.md docs/graph-report.md && git add docs/graph-report.md && git commit -m "docs: refresh graph report snapshot"`
