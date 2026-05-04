<!-- Save this file as `.github/PULL_REQUEST_TEMPLATE.md` -->

> ## 🚫 STOP — read before submitting
>
> **This project is pre-1.0 and is not currently accepting external implementation PRs.** The architecture is still being established; accepting outside contributions during the design phase would slow everything down. See the [README](../README.md) and [CONTRIBUTING.md](../CONTRIBUTING.md) for current contribution policy.
>
> **What we DO accept right now:** typo fixes in docs, fixes to broken links, trivial doc improvements.
>
> **What we DO NOT accept right now:** any change to `src/`, `include/`, `tests/`, build files, or CI config from external contributors.
>
> If you have a feature you want in v1.0, please **file an Issue first** rather than submitting a PR. Well-argued requests can shape v1.0 scope.
>
> Maintainer PRs (those by users listed in `CODEOWNERS`) may proceed normally.
>
> ---
> If your PR fits the "DO accept" category above, delete this STOP block and continue with the template below.

## Description
<!-- What does this PR do? Brief — one paragraph. -->

## Related issue / ICP
<!-- closes #NN, addresses ICP-NNNN, or "no related issue" -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor (no behavior change — please add the `[refactor]` label)
- [ ] Documentation
- [ ] Test infrastructure
- [ ] Build / CI / packaging
- [ ] Breaking change (interface contract changed — ICP required, see `docs/icps/`)

## Tests

- [ ] Tests added or updated for any `src/` change *(or* `[refactor]` *label set with explanation)*
- [ ] Tests run locally and pass
- [ ] Diff coverage thresholds expected to be met (90% SDK / 80% transport)

## Review checklist

<!-- Reviewers walk this list. PR author can pre-check items they've verified. -->
<!-- Source of truth: docs/coding-standards.md §14 -->

- [ ] **RAII discipline**: no raw `new`/`delete`; ownership clear; no smart-mock anti-patterns
- [ ] **Complexity**: cognitive ≤ 15, cyclomatic ≤ 10, indentation ≤ 3 levels per function
- [ ] **Public API**: Doxygen comment with threading guarantee and error/exception behavior
- [ ] **No banned constructs**: no `goto`, no naked `catch(...)`, no unsafe C functions, no `using namespace` in headers, no commented-out code
- [ ] **Const correctness**: methods, parameters, locals marked `const` where applicable
- [ ] **Naming**: PascalCase types/functions, snake_case locals/params, `m_` member prefix, `k`-prefix constants
- [ ] **No magic numbers**: named `constexpr` constants
- [ ] **Threading category** stated for any new type (thread-safe / thread-affine / externally synchronized)
- [ ] **Includes** ordered per coding-standards.md §9.1 (clang-format auto-applies)

## Breaking-change checklist

<!-- Skip if this is not a breaking change. -->

- [ ] ICP filed in `docs/icps/`
- [ ] Migration path documented in the ICP
- [ ] Affected components enumerated
- [ ] Compatibility-matrix updated if user-visible

## Notes for the reviewer

<!-- Anything specific you want the reviewer to look at, design alternatives you considered, etc. -->
