# `src/common/raii/`

## What lives here

Header-only RAII wrappers for the C resources microtel owns. Each one is
move-only, has a `noexcept` destructor, and lives in namespace
`microtel::common::raii`.

They are layer-independent (the transport, the wire codecs and anything else
that calls a C library use them), but Track D owns their review because the
transport touches most of them.

## Owner

Track D — Transport (review). Used by every track that touches a
C resource.

## What it implements

The wrappers required by `microtel-spec.md` §14.3 and listed in
`docs/memory-model.md` §4.2, plus a few that arrived later:

| Wrapper | Header | Owns | Destructor calls |
|---|---|---|---|
| `UniqueFd` | [`unique_fd.hpp`](unique_fd.hpp) | `int` file descriptor | `close(2)` |
| `SslCtx` | [`ssl_ctx.hpp`](ssl_ctx.hpp) | `SSL_CTX*` | `SSL_CTX_free` |
| `SslSession` | [`ssl_session.hpp`](ssl_session.hpp) | `SSL*` | `SSL_free` |
| `Nghttp2Session` | [`nghttp2_session.hpp`](nghttp2_session.hpp) | `nghttp2_session*` | `nghttp2_session_del` |
| `BioMethod` | [`bio_method.hpp`](bio_method.hpp) | `BIO_METHOD*` | `BIO_meth_free` |
| `DeflateStream` | [`deflate_stream.hpp`](deflate_stream.hpp) | initialised `z_stream` | `deflateEnd` |
| `InflateStream` | [`inflate_stream.hpp`](inflate_stream.hpp) | initialised `z_stream` | `inflateEnd` |

`BioMethod` is not in the spec's list. It arrived with the SIGPIPE-safe TLS
BIO (issue #177) and owns that BIO type's method table. The `BIO` objects
built from it are owned by the `SSL` they are handed to, not here.

The two zlib streams hold their `z_stream` behind a `unique_ptr` so its address
survives a move. zlib keeps a back-pointer to the `z_stream` it was initialised
against, and a by-value member leaked 42 KiB per moved stream under
LeakSanitizer (first on `InflateStream`, then issue #186 on `DeflateStream`).

The `UpbArena` wrapper lives in [`src/wire/encoder/`](../../wire/encoder/),
not here, because it must not escape that directory (LOCKED —
`memory-model.md` §3.1).

## Dependencies

- OpenSSL, nghttp2 and zlib (system libraries; dev headers from
  `openssl-devel`, `libnghttp2-devel` and `zlib-devel`).

## Tests

- `tests/unit/common/raii/`: `ssl_ctx_test.cpp`, `ssl_session_test.cpp`,
  `nghttp2_session_test.cpp`, `deflate_stream_test.cpp` and
  `inflate_stream_test.cpp` check move-only semantics, `Release()` or `Reset()`
  correctness, a no-op destructor in the empty state, and no leaks under ASan.
  `UniqueFd` and `BioMethod` have no dedicated test file; they are exercised
  through `tests/unit/transport/` and `tests/integration/transport/`.

## Style notes

- **Move-only by default** (LOCKED — `memory-model.md` §4.2). Copy
  constructor and copy assignment are `= delete`.
- **`noexcept` destructor** (LOCKED). Always.
- `Release()` returns the underlying handle and leaves the wrapper empty, so
  the destructor does nothing (`UniqueFd` holds `kInvalid`, i.e. -1; the
  pointer wrappers hold `nullptr`). The zlib streams have no handle to give
  back, so their release verb is `Reset()`, which ends the stream in place.
- No copyable shadow: no `clone()` and no implicit conversion to the
  underlying handle. Code that needs the raw handle calls `Get()`.
- **Rule of zero or rule of five** (LOCKED — `coding-standards.md` §5).
  Never the compiler-generated mix.
- No `goto` in cleanup paths; RAII makes it unnecessary.
- No raw `new`/`delete`. The wrappers use the C libraries' own allocators,
  which is the point of having them; production C++ code uses
  `std::make_unique`.
