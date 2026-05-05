# `src/common/config/`

## Purpose

Parses `microtel.toml`, resolves env-var overlays, validates, freezes,
and hands a final `Config` value back to `SdkBuilder`. Plus the two
auth-provider implementations.

The frozen `Config` is the input every other component sees during
construction. There is no runtime mutation of config in v1; hot reload
is a v1.1+ feature.

## Owner

Track E — Configuration.

## Implements

- The TOML parser usage (parser library selected in M2 chunk 5)
- Env-var reading per the precedence in `docs/configuration.md` §1
- The resolved `Config` value type
- Validation: every entry in `docs/configuration.md` §3 has its
  validator here, mapping failures to `ConfigError::Kind`
- `internal::IAuthProvider` realisations: `StaticHeadersAuthProvider`
  and `CallbackAuthProvider` (with TTL cache)

## Depends on

- A TOML parser library (selection in M2 chunk 5; candidates: `toml++`,
  `cpptoml`, both header-only)
- `microtel::Error`, `microtel::ConfigError` from
  [`src/common/`](../) base headers

## Test entry points

- `tests/unit/common/config/` — every setting has its precedence (code
  vs env vs file vs default), validation (happy + every documented
  failure), and edge-case test.
- `tests/unit/common/auth/` — static-headers and callback paths,
  TTL-cache behaviour.
- `tests/fuzz/toml_fuzz.cpp` — adversarial TOML inputs (M9 hardening).

## Style notes

- **Eager validation.** `SdkBuilder::Build` validates at construction
  time and returns `microtel::Expected<Provider, ConfigError>`. No
  network preflight here — that's the `microtel --preflight` CLI
  surface (`docs/error-model.md` §8).
- **Strict by default.** Unknown TOML keys raise `ConfigError::UnknownKey`
  unless `[config] unknown_keys = "warn" | "ignore"`.
- **Auth callback runs on the exporter worker thread** (LOCKED —
  `interfaces.md` §4.9). Document this in the public
  `WithAuthProvider` API; user code must be thread-safe.
- **Secret redaction at the boundary** per `docs/configuration.md` §5.
  The resolved-config dump emitted at `info` level redacts
  `Authorization` headers, client secrets, token-provider outputs.
- **Env-var precedence** per `docs/configuration.md` §1: code > env >
  file > defaults. `MICROTEL_*` wins over `OTEL_*` on overlap; v1
  defines no overlap, so the rule is dormant but documented.
- **Frozen at `Build` time.** No mutation API. Hot reload arrives in
  v1.1's control plane.
