# ICP 0038: `Build()` rejects user headers that HTTP/2 forbids or that microtel sets itself

**Status:** Draft
**Affected interfaces / docs:**
- `src/common/config/config_validator.cpp` (`Validate` gains a header check)
- `include/microtel/sdk_builder.hpp` (Doxygen on `WithHeaders` and
  `WithAuthProvider`; no signature change)
- `docs/configuration.md` §3.3 (the `[exporter.headers]` row)
- `docs/error-model.md` §8 (a new init-failure row)
- `docs/grpc-wire-protocol.md` §2.1 (the header-rules paragraph and the
  `:authority` sentence)
- `docs/troubleshooting.md` (the `Build()` error messages)

**Affected tracks:** SDK / config. No change to the wire codecs, the
transport, `ConfigError::Kind`, `DropReason` or any public signature.

## Summary

`SdkBuilder::Build()` fails with `ConfigError::Kind::InvalidValue` when a
static request header (`WithHeaders`, `OTEL_EXPORTER_OTLP_HEADERS`,
`[exporter.headers]`) has a name that HTTP/2 forbids in a request, or a name
microtel sets itself for the resolved protocol.

## Motivation

Issue #408. The codecs build their own headers and then append the user's
headers unchanged; nghttp2 sends them unchanged too, apart from lowercasing
the names. Nothing checks them. `docs/grpc-wire-protocol.md` §2.1 claimed a
user `host` header was rejected at config load; #406 corrected the doc, not
the behaviour.

A probe against the pinned collector (`otel/opentelemetry-collector-contrib:0.160.0`)
and a python-h2 server that logs the exact header list (results in #408)
found:

- **Pseudo-headers (`:authority`, `:path`)** reach the wire after the regular
  headers. Both receivers answer with a connection-level `PROTOCOL_ERROR`,
  which microtel classifies as transient. So every export burns the whole
  retry budget: 8.6 s and 5 connections before it fails.
- **Connection-specific headers** (`connection`, `keep-alive`,
  `transfer-encoding`, `upgrade`, and `te` other than `trailers`): every
  export fails on HTTP/protobuf with HTTP 400. On gRPC, `connection` fails;
  the others are tolerated by grpc-go, but RFC 9113 §8.2.2 makes all of
  them malformed.
- **Names microtel already sets** (`content-type`, `content-length`, `te`,
  `user-agent`) go out twice. The collector tolerates the duplicate; the
  h2 server rejected a conflicting `content-length` outright, and microtel
  retried until the connection was lost.
- **`host`** is sent beside `:authority`. Both Go receivers ignore it.

A header that cannot work should fail where the operator sees it, at
`Build()`, not as a retry storm logged as a transport error.

## Proposed change

`Validate` (`src/common/config/config_validator.cpp`) runs after the protocol
is resolved, so the check can depend on it. For each static header, names
compared ASCII case-insensitively (as `table_merge` already does):

1. **The name must be a non-empty RFC 9110 token.** This rejects
   pseudo-headers (a leading `:`), empty names, and names containing
   spaces or other separators.
2. **Forbidden in any HTTP/2 request** (RFC 9113 §8.2.2, §8.3.1):
   `connection`, `keep-alive`, `proxy-connection`, `transfer-encoding`,
   `upgrade`, `te`, `host`.
   - `te: trailers` is the one legal `te` value. microtel already sends it
     on gRPC, and it means nothing on HTTP/protobuf, so all `te` is
     rejected.
   - `host` would only duplicate or contradict `:authority`, which comes
     from the endpoint. Rejecting it is what §2.1 used to promise.
3. **Set by microtel for the resolved protocol:**
   - gRPC: `content-type`, `user-agent`, `grpc-encoding`,
     `grpc-accept-encoding`.
   - HTTP/protobuf: `content-type`, `content-length`, `content-encoding`,
     `accept-encoding`.

   The encoding headers are reserved even when compression is off: a
   user `content-encoding` with an uncompressed body is a lie. A name
   microtel does not set on that protocol stays allowed. For example,
   `user-agent` on HTTP/protobuf is the only `user-agent` sent, and
   `grpc-timeout` is never set by microtel.
4. **`authorization` when `WithAuthProvider` is set:** the callback's
   header would be a second `authorization`. A static `authorization`
   with no callback stays allowed. It is the common way to pass a fixed
   bearer token through `OTEL_EXPORTER_OTLP_HEADERS`.

The failure:

- `kind` is `InvalidValue`.
- `field` is `exporter.headers.<name as given>`.
- `message` names the header and the reason class, for example
  `header "host" is set by microtel from the endpoint`. Header values are
  never included, because they may be secrets.

The first offending header fails the build.

Unchanged:

- **`WithExportTransport` path:** headers set in code already fail
  `Build()`; headers from the environment or file stay ignored and
  unvalidated (`configuration.md` §3.15).
- **The auth callback:** it supplies only the `authorization` value, never
  a name, so there is nothing to check at request time.
- **Header values:** this ICP checks names only. Values with CR, LF or
  NUL are a separate question (see the end).

## Migration

**For users:** a configuration that set one of these headers used to build;
after this change `Build()` fails, naming the header. The fix is to drop
the header:

- **Pseudo-headers and connection-specific headers:** every export with
  them already failed against a Go receiver.
- **`host`:** has no effect today.
- **The duplicates:** microtel's own value is the one receivers use
  anyway.

The change ships in a minor release (1.3.0) and is called out in its
release notes as a behaviour change. `RELEASING.md` §7 keeps the public
C++ API source-stable within a major version. No signature changes here,
but configurations that built before can now fail.

**For contributors:** tests come first. These are the success criteria:

- **Unit tests** in `tests/unit/common/config/`:
  - each rejected name is rejected, on the protocol it applies to;
  - case-insensitivity;
  - the `authorization` + `WithAuthProvider` pair;
  - names reserved on one protocol stay accepted on the other;
  - an ordinary `x-tenant` header still builds.
- **An `sdk_builder` test** that the env and TOML sources hit the same
  check.
- **A conformance test** that a custom header still reaches the
  collector on both protocols.

## Rationale & alternatives

- **Normalise instead of rejecting** (lowercase, drop or overwrite
  clashes with a `Warn`). It is friendlier, but it silently sends
  something other than what the operator configured, and a `Warn` is
  easy to miss. Lowercasing is already done by nghttp2 and needs
  nothing.
- **Document and leave it.** That leaves the pseudo-header retry storm in
  place for a mistake that `Build()` can see.
- **Let users override microtel's headers** (for example a custom
  `user-agent` on gRPC replacing `microtel-cpp/<version>`). No one has
  asked for it. If someone does, it is an explicit option, not an
  accident of header order.
- **Check at the codec (request time).** The headers are static, so
  `Build()` sees everything the codec would, and it fails once, early,
  with a `field` path.

## Open questions

1. **Value checks.** Should `Build()` also reject header values containing
   CR, LF or NUL? RFC 9113 §8.2.1 makes them malformed. The #408 probe did
   not test values. Proposed: out of scope here; open a follow-up issue if
   wanted.
2. **gRPC `user-agent`.** gRPC's spec lets a client prepend its own
   `user-agent` to the library's. Reject it (proposed), or later add an
   explicit option to prepend a user-agent product token?
