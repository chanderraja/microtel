# ICP 0029: auth-provider caller-count correction

**Status:** Accepted — decided 2026-09-16. Docs and one header comment; no code
change.
**Affected interfaces / docs:** [`docs/interfaces.md`](../interfaces.md) §4.9
(one LOCKED sentence),
[`include/microtel/internal/auth_provider.hpp`](../../include/microtel/internal/auth_provider.hpp)
(`@threadsafety` comment). No signature, ABI, or wire change. No migration.
**Affected tracks:** documentation only.

## Motivation

§4.9's Threading bullet reads "Must be thread-safe (the cache update path) but
in practice only one caller." That was true when the trace exporter was the
only exporter. Since M12/M14, `SdkBuilder::BuildExporters` hands the same
auth provider to the trace, metric, and log codecs, so up to three exporter
workers call `GetAuthorization` concurrently — and the callback runs under the
provider's mutex, so a slow callback stalls all three pipelines, not one.
Issue #252 records the staleness; [`docs/auth-callback-recipes.md`](../auth-callback-recipes.md)
already documents the operational consequence.

The **requirement** — thread-safety — was always stated and is unchanged. User
code that met the documented contract is unaffected; only the "in practice"
reassurance beside it was wrong, and a reassurance that understates concurrency
is exactly the kind of stale LOCKED text worth an ICP rather than a drive-by
edit (the ICP 0009 precedent: correct the sentence, keep a "previously read"
note).

## Proposed change

1. §4.9's Threading bullet drops "but in practice only one caller" in favour of
   the true caller set (three exporter workers sharing one provider, callback
   under the provider mutex), with a note recording what the sentence
   previously read.
2. `auth_provider.hpp`'s `@threadsafety` line drops "(in practice:
   single-caller — exporter worker)" for the same statement, so the header and
   the contract doc agree.

## Rejected alternative

Fixing only the non-LOCKED header comment was rejected when #252 was triaged in
PR #275: it would make the header contradict the contract document, which is
worse than both being consistently stale.
