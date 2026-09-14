> ## Before you submit
>
> Thanks for contributing — [CONTRIBUTING.md](../CONTRIBUTING.md) is the full version of what follows.
>
> - **Tests first.** Any change under `src/**/*.{cpp,hpp}` needs a matching change under `tests/` in the same PR. The one override is the `[refactor]` label — spelled with the brackets, applied to the PR — and only for changes that really are behaviour-preserving (also the escape hatch for comment-only and formatting-only diffs). Deletions are exempt automatically.
> - **Run the gates locally** before pushing: `CLANG_FORMAT=clang-format ci/scripts/format-check.sh`, then `ci/scripts/tidy-check.sh build`. Both fail the build, and unformatted PRs never reach a reviewer.
> - **Changing a locked interface?** That needs an [Interface Change Proposal](../docs/icps/) — a short markdown document PR'd into `docs/icps/`, not an argument in review comments. Same for any new runtime dependency.
>
> Delete this block once you've read it, and fill in the template below.

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
