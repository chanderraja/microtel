# ICP 0013: Retire M8, re-add Python bindings as M18 covering all three signals

**Status:** Draft
**Affected interfaces / docs:** `microtel-spec.md` §6.2, §13 milestone table (M8, M10, new M18), §13.4, §13.5; `microtel-roadmap.md` §4 (v1.0 theme). No interface headers, no `docs/interfaces.md`.
**Affected tracks:** none — milestone scope and documentation only.

## Summary

Retire the **M8 – Python bindings (traces only)** milestone, re-add Python
bindings as **M18** covering traces, metrics, and logs, and drop Python from the
v1.0 target set.

## Motivation

Two things changed since M8 was written.

**M8's scope statement is now false.** It specifies `microtel.__capabilities__`
= `{"traces": True}` with "metrics/logs raise `NotImplementedError` with the
version target." Metrics shipped in M12, Views in M13, Logs in M14 — all three
are live public C++ surfaces today (`Provider::GetTracer` / `GetMeter` /
`GetLogger`). A traces-only wheel would violate §13.4's own rule: *"No orphan
C++ surfaces — if a public C++ method exists for more than one release without a
Python equivalent, that's a bug, not a deferred feature."* Under the current
text, M8 ships the bug it forbids.

**M8 occupies a slot the build never used.** ICP 0010's secondary observation
recorded that M8 has no implementation — history goes M7 → M9 — and noted that
"a future ICP can restate M8's target version if desired." This is that ICP.

Deferring rather than rescoping in place is the honest option, because two
prerequisites are unbuilt:

1. **No thread-local current-span slot.** `SdkTracer::StartAsCurrentSpan`
   ([`src/sdk/sdk_tracer.cpp:141`](../../src/sdk/sdk_tracer.cpp)) delegates to
   `StartSpan`, commented *"Thread-local context propagation machinery deferred
   to v1.1."* The §6.2 Python example is built on `with
   tracer.start_as_current_span(...)`; without the slot the `with` block ends
   the span correctly but child spans are not implicitly parented. Relatedly,
   `SdkProvider` passes `nullptr` for `ICurrentSpanSource`
   ([`src/sdk/sdk_provider.cpp:237`](../../src/sdk/sdk_provider.cpp)), so
   `SdkLogger::FillTraceContext`
   ([`src/sdk/sdk_logger.cpp:32`](../../src/sdk/sdk_logger.cpp)) returns early
   and log↔trace correlation never fires. A Python `logging` bridge without
   correlation is not worth shipping.
2. **No linkable artifact.** `src/` builds ten separate `STATIC` libraries with
   no aggregate target and no `POSITION_INDEPENDENT_CODE`; non-PIC static
   objects cannot be linked into a shared Python extension module. This overlaps
   issue #19 (`install(TARGETS … EXPORT …)`).

## Proposed change

All edits are to `microtel-spec.md` and `microtel-roadmap.md`.

1. **§13 table — delete the M8 row.** M8 becomes a **retired number**: never
   reused, so every as-built commit label M9–M16 keeps its meaning. Same
   no-force-push constraint that drove ICP 0010.
2. **§13 table — append M18:**

   | Milestone | Scope | Effort | Depends on | Parallel with |
   |---|---|---|---|---|
   | **M18 – Python bindings (all signals)** | nanobind layer over traces, metrics, and logs; `logging.Handler` bridge; wheels via `cibuildwheel` for manylinux_2_28; examples. `microtel.__capabilities__` reports `{"traces": True, "metrics": True, "logs": True}`. | 4 wk | M10; thread-local current-span slot | M15, M16, M17 |

   Effort rises from 3 wk to 4 wk because the surface triples.
3. **§13 table — M10 row:** strike "PyPI publish" from M10's scope; it moves to
   M18.
4. **§13.4 rewrite:** replace "v1 wheels include traces only" with the
   all-signals statement. Keep the no-orphan-surfaces rule verbatim — it is the
   rule that motivated this ICP.
5. **§13.5 release gates:** replace "Python bindings (M8) are a v1.0 *target*
   but not a gate; if Python slips, C++ ships v1.0 and Python ships v1.0.1"
   with: Python is not part of the v1.0 target set and ships post-v1.0 as M18.
6. **§6.2:** add a one-line note that the example's `start_as_current_span` is
   the M18 target shape and requires the thread-local current-span slot, which
   is not yet implemented. The example itself stays as designed — it is the
   intended API, not a claim about today.
7. **`microtel-roadmap.md` §4, v1.0 theme:** remove the "Python bindings
   (target, not blocker)" bullet. The Tier 3 row naming `microtel_python_shim`
   is untouched — that shim registers against `opentelemetry-api` and is a
   different artifact from the nanobind bindings.

## Migration

- Contributors and agents: **M8 is retired — do not reuse the number.** Python
  bindings are **M18**.
- v1.0 ships C++ only: no wheel and no PyPI publish in the v1.0 gate set.
- No code, interface, or test changes. Nothing to rebuild.

## Rationale & alternatives

- **Rescope M8 in place and keep it inside v1.0** — rejected. Both
  prerequisites are unbuilt, and the slot has never corresponded to real work;
  keeping it in v1.0 preserves a gate that has been silently slipping since M7.
- **Ship traces-only per the existing text** — rejected. It violates §13.4's
  no-orphan-surfaces rule against three live signals, and hands Python users an
  API strictly weaker than the C++ one for no reason beyond a stale sentence.
- **Renumber M9–M16 to close the M8 hole** — rejected for ICP 0010's reason:
  merged, branch-protected history cannot be renumbered without a force-push.
- **Fold Python into M16 (sugar layer)** — rejected. M16's Python half assumes
  bindings already exist; merging the two produces one oversized milestone with
  an internal ordering constraint.

## Related, not proposed here

The `opentelemetry-cpp` API-adapter shim (`microtel_otelcpp_shim`, spec §6.3) is
unimplemented despite roadmap §3's tier table listing Tier 3 as "experimental:
traces" from v1.0. It does **not** share the current-span-slot prerequisite —
the opentelemetry-cpp API ships its own thread-local context storage
(`ThreadLocalContextStorage`, `api/include/opentelemetry/context/runtime_context.h`),
so `RuntimeContext` and `trace::Scope` resolve inside the shim.

That scheduling decision is now made in
**[ICP 0014](0014-otelcpp-shim-and-rule-13.md)**, which takes the shim as **M17**
— ahead of Python — because it has no SDK prerequisites while Python has one.
ICP 0014 also covers the build-time dependency on the opentelemetry-cpp API
headers (CLAUDE.md rules 12–13) and the ABI-configuration constraint those
headers impose. Python was renumbered here from M17 to **M18** so the numbers
track intended execution order; both ICPs are drafts, so no merged milestone
label is affected.
