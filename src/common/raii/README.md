# `src/common/raii/`

## Purpose

Custom RAII wrappers for the C resources microtel owns. Each wrapper is
move-only, has a `noexcept` destructor, and exposes `release()` for
explicit ownership transfer across API boundaries.

These types are layer-independent (used by `src/transport/`,
`src/wire/encoder/`, and anywhere else microtel calls a C library), but
their review is owned by Track D because the transport touches the most
of them.

## Owner

Track D — Transport (review). Used by every track that touches a
C resource.

## Implements

The wrappers required by `microtel-spec.md` §14.3 and listed in
`docs/memory-model.md` §4.2:

| Wrapper | Owns | Destructor calls |
|---|---|---|
| `Socket` | `int` file descriptor | `close(2)` |
| `SslCtx` | `SSL_CTX*` | `SSL_CTX_free` |
| `SslSession` | `SSL*` | `SSL_free` (with clean shutdown if reachable) |
| `Nghttp2Session` | `nghttp2_session*` | `nghttp2_session_del` |

The `UpbArena` wrapper lives in [`src/wire/encoder/`](../../wire/encoder/),
not here, because it must not escape that directory (LOCKED —
`memory-model.md` §3.1).

## Depends on

- OpenSSL, nghttp2 (system libraries, dev headers from
  `libnghttp2-devel` and `openssl-devel`).

## Test entry points

- `tests/unit/common/raii/` — one file per wrapper. Verifies move-only
  semantics, `release()` correctness, no-op destructor on the released
  state, leak-free under ASan.

## Style notes

- **Move-only by default** (LOCKED — `memory-model.md` §4.2). Copy
  constructor and copy assignment are `= delete`.
- **`noexcept` destructor** (LOCKED). Always.
- **`release()` returns the underlying handle** and leaves the wrapper
  in a documented "empty" state where the destructor is a no-op (e.g.,
  `Socket` empty-state is `m_fd = -1`; `Nghttp2Session` empty-state is
  `m_session = nullptr`).
- **No copyable shadow** — no `clone()`, no implicit conversion to the
  underlying handle. Code that needs the raw handle calls a named
  accessor (`get()` or `native_handle()`).
- **Rule of zero or rule of five** (LOCKED — `coding-standards.md` §5).
  Never the compiler-generated mix.
- **No `goto`** in cleanup paths. RAII obviates it.
- **No raw `new`/`delete`.** Wrappers use C library allocators (which is
  the whole point); production C++ code uses `std::make_unique`.
