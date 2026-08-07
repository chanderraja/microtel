# ICP 0012: Add `Provider::GetLogger` (M14 L5)

**Status:** Draft
**Affected interfaces / docs:** `include/microtel/provider.hpp` (new pure-virtual `GetLogger`), `include/microtel/logger.hpp` (unchanged; realised by an SDK no-op logger), `include/microtel/sdk_builder.hpp` (log-exporter configuration), `src/sdk/sdk_provider.*` (implementation), `docs/logs-design.md` §8.
**Affected tracks:** A (SDK). No transport / encoder / wire changes.

## Summary

Add `GetLogger(name, version)` to the public `Provider` interface so
applications can obtain a `Logger`, and wire `SdkProvider` / `SdkBuilder` to
lazily construct the L4 log pipeline behind it.

## Motivation

L4.1–L4.3 (merged) built the entire SDK-internal log pipeline —
`SdkLogger → ILogRecordProcessor → ILogExporter → ILogEncoder → IWireCodec` —
but nothing exposes or constructs it. `Provider::GetLogger` is the missing entry
point, mirroring `GetTracer` and `GetMeter`. Because `Provider` is a locked
public interface (M0), adding a method to it requires an ICP per the process,
which is what `docs/logs-design.md` §8 flagged.

## Proposed change

Add to `include/microtel/provider.hpp`, alongside `GetTracer` / `GetMeter`:

```cpp
/// @brief Acquire (or create) the Logger for one instrumentation scope.
/// Returns a no-op logger when no logs exporter is configured.
[[nodiscard]] virtual std::shared_ptr<Logger>
GetLogger(std::string_view name, std::string_view version = {}) = 0;
```

- **Pure virtual**, matching `GetTracer` and `GetMeter` (both `= 0`).
- `SdkProvider::GetLogger` lazily constructs the log pipeline (exporter +
  processor) on first call — mirroring how `GetMeter` lazily starts the metrics
  pipeline — and caches an `SdkLogger` per `(name, version)`. When no log
  exporter is configured it returns a shared no-op `Logger`.
- `SdkBuilder` gains log-exporter configuration (`WithLogExporter(...)`, or
  auto-enable when a logs endpoint is configured), threading a log exporter and
  logs `IWireCodec` into `SdkProviderArgs`.
- A no-op `Logger` (whose `Emit` drops) is added under `src/sdk/` for the
  logs-disabled path, analogous to `NoopSpan`.

The substantive edits land in the L5 implementation PR, which references this
ICP by number. This ICP merges first.

## Migration

- The only in-tree `Provider` implementer is `SdkProvider`, updated in the L5
  PR. There are **no `Provider` mocks or fakes** in the tree, so nothing else
  needs touching.
- External code that subclasses `Provider` directly must add a `GetLogger`
  override. This is the same one-time break `GetMeter` introduced for the
  metrics signal; pre-1.0 for the logs signal, it is acceptable and keeps the
  three signal accessors uniform.
- Callers using the `Provider` interface are unaffected — `GetLogger` is
  additive to the vtable.

## Rationale & alternatives

- **Pure virtual (chosen).** Uniform with `GetTracer` / `GetMeter`; implementers
  must be explicit; the break is confined to the single internal `SdkProvider`.
- **Virtual with a default no-op body (rejected).** Non-breaking, but lets a
  provider silently swallow logs and diverges from the two sibling accessors
  that are pure virtual. The uniformity is worth the contained break.
- **A separate `LoggerProvider` type (rejected).** microtel deliberately uses
  one unified `Provider` for traces, metrics, and logs; a second provider type
  would fragment lifecycle (`Connect` / `ForceFlush` / `Shutdown`) ownership.
