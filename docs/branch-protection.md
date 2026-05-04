# Branch Protection and Repository Settings

This document records the GitHub-side configuration that enforces the contribution policy described in [CONTRIBUTING.md](../CONTRIBUTING.md).

The intent is **layered signaling**, not hard blocking. PRs from outside contributors aren't disabled — that would be hostile to the OSS framing — but several layers prevent unwanted PRs from landing accidentally.

---

## Layer 1: Documentation signals (already in place)

- README.md has a prominent "🚫 Not accepting external implementation PRs until v1.0" banner near the top.
- CONTRIBUTING.md "Project status" section makes the policy explicit, with a table showing what's welcome vs not.
- `.github/PULL_REQUEST_TEMPLATE.md` opens with a STOP preamble that any PR author sees as soon as they hit "Open pull request."

These three signals catch maybe 90% of well-meaning contributors. The remaining layers handle the rest.

---

## Layer 2: Branch protection on `main`

Configure under **Settings → Branches → Branch protection rules → Add rule** for the `main` branch:

| Setting | Value | Reason |
|---|---|---|
| Require a pull request before merging | ✅ | No direct pushes to `main` |
| Require approvals | ✅ 1 approval | Maintainer must approve |
| Dismiss stale approvals when new commits are pushed | ✅ | Force re-review on changes |
| Require review from Code Owners | ✅ | CODEOWNERS routes review |
| Require status checks to pass before merging | ✅ | CI gates from `ci-architecture.md` |
| Require branches to be up to date before merging | ✅ | No stale PRs |
| Required status checks | `build-and-test`, `static-analysis`, `sanitizers`, `coverage`, `test-presence`, `sonarqube` | Per `ci-architecture.md` |
| Require conversation resolution before merging | ✅ | All review comments addressed |
| Require signed commits | optional pre-1.0 | Tighten before public release |
| Require linear history | ✅ | No merge commits in `main` |
| Require deployments to succeed before merging | n/a | Not deploying to anywhere |
| Lock branch | ❌ | Don't lock; we want maintainers to merge |
| Do not allow bypassing the above settings | ✅ | Even admins use PRs |
| Restrict who can push to matching branches | ✅ — only `@TBD-username` | Matches CODEOWNERS |

Result: nothing gets into `main` without the maintainer's approval. External PRs can be opened but not merged unilaterally.

---

## Layer 3: Repository settings

### Settings → General

| Setting | Value | Reason |
|---|---|---|
| Issues | ✅ enabled | Issues are how we want feedback |
| Discussions | ✅ enabled | For broader architecture conversations |
| Projects | ✅ enabled | For roadmap tracking |
| Wikis | ❌ disabled | Docs live in `/docs` |
| Sponsorships | optional | Decide pre-1.0 |
| Preserve this repository | ✅ | Archive eligibility |

### Settings → Pull Requests

| Setting | Value | Reason |
|---|---|---|
| Allow merge commits | ❌ | Linear history |
| Allow squash merging | ✅ (default) | Squash with PR-title message |
| Allow rebase merging | ✅ | For trivial PRs |
| Always suggest updating pull request branches | ✅ | UX for contributors |
| Allow auto-merge | ✅ | Maintainer convenience |
| Automatically delete head branches | ✅ | Cleanup |

### Settings → Moderation → Code review limits

This is **the closest GitHub gets to "don't let strangers PR me."** Find it under Settings → Moderation → Code review limits.

| Setting | Value | Reason |
|---|---|---|
| Limit who can review pull requests | "Users that have read access" (default) | Don't restrict reviewers |
| Limit who can submit pull requests | **"Users that have explicitly been granted access"** | This blocks random PRs |

**This is the practical lever.** Setting "Limit who can submit pull requests" to "explicitly granted access" means:

- Existing collaborators (you, plus anyone you've added) can submit PRs.
- Anyone else who tries gets blocked at PR submission with a message about repository access.
- It doesn't disable the fork button, but PRs from forks get rejected.

You can lift this restriction when v1.0 ships and you're ready to accept external contributions.

### Settings → Moderation → Interaction limits

GitHub also offers "Interaction limits" under Settings → Moderation → Interaction limits. Options:

- **"Limit to existing users"** (no accounts < 24 hours old)
- **"Limit to prior contributors"** (users who've previously committed)
- **"Limit to repository collaborators"** (only your CODEOWNERS-listed users)

Each lasts up to 6 months (renewable). Use **"Limit to repository collaborators"** as the strongest signal. Combined with the code-review-submission limit above, this is belt-and-suspenders.

---

## Layer 4: CODEOWNERS as the routing layer

[CODEOWNERS](../CODEOWNERS) is the file that GitHub uses to auto-request reviews. Currently it routes everything to `@TBD-username` (you). Even if a PR is somehow opened, you're auto-requested as the reviewer and the merge is gated on your approval per Layer 2.

When the team grows post-v1.0, replace `@TBD-username` with the relevant track owners.

---

## Layer 5: GitHub Actions permissions

Under **Settings → Actions → General → Workflow permissions:**

| Setting | Value | Reason |
|---|---|---|
| Default workflow permissions | `Read repository contents and packages permissions` | Principle of least privilege |
| Allow GitHub Actions to create and approve pull requests | ❌ | Prevents auto-merge bots from approving |

Under **Settings → Actions → General → Fork pull request workflows:**

| Setting | Value | Reason |
|---|---|---|
| Run workflows from fork pull requests | ✅ but require approval for first-time contributors | Saves CI minutes on spam |
| Send write tokens to workflows from fork pull requests | ❌ | Security: forked PRs run with read-only tokens |
| Send secrets and variables to workflows from fork pull requests | ❌ | Security: secrets not exposed to forks |

---

## Layer 6: Issue & PR templates

`.github/ISSUE_TEMPLATE/config.yml` is configured to redirect common questions:

- **Spec questions** → microtel-spec.md
- **Roadmap questions** → microtel-roadmap.md
- **Contribution questions** → CONTRIBUTING.md
- **Security issues** → SECURITY.md (private disclosure)

Blank issues are disabled. Anyone opening an issue picks a template, which forces them to think about which kind of feedback they're providing.

---

## What this configuration achieves

- **Strangers can't submit implementation PRs.** Code review submission limit blocks them.
- **Existing contributors (you) can still PR normally.** Layer 5 doesn't apply to you.
- **CI minutes don't get burned by spam PRs.** Fork workflow approval requirement.
- **Secrets stay safe.** Forks don't get write tokens or secrets.
- **You're auto-requested as reviewer for everything.** Via CODEOWNERS.
- **Nothing merges without you.** Branch protection requires your approval.
- **External contributors who really want to engage are funneled to Issues.** Via templates and config.yml.

This is the right set of layers for a **public** pre-1.0 OSS project. If you keep the repo private until v1.0, none of this matters because no one but invited collaborators can see it anyway.

---

## When v1.0 ships

Open up the gates:

1. Lift the "code review submission limit" — set back to "anyone with read access."
2. Lift "interaction limits" if set.
3. Update README.md to remove the "not accepting PRs" callout.
4. Update CONTRIBUTING.md's "Project status" section to drop the no-PRs language.
5. Update `.github/PULL_REQUEST_TEMPLATE.md` to remove the STOP preamble.
6. Keep all other branch-protection settings — those are still good practice post-1.0.
7. Update CODEOWNERS to add other track owners as the team grows.

---

## Settings checklist for first-time setup

When you create the repo:

```
[ ] Make repo public
[ ] Add LICENSE (Apache 2.0)
[ ] Add NOTICE (placeholder)
[ ] Push initial doc bundle (CLAUDE.md, spec, roadmap, etc.)
[ ] Settings → Branches → Branch protection rule on `main` per Layer 2
[ ] Settings → Moderation → Code review limits → "explicitly granted access"
[ ] Settings → Moderation → Interaction limits → "limit to collaborators"
[ ] Settings → Actions → Workflow permissions per Layer 5
[ ] Add SONAR_TOKEN to repo secrets
[ ] Verify CODEOWNERS triggers review request on a test PR (use a draft PR from a branch)
[ ] Set up SonarQube Cloud project in OSS tier
```
