# ICP 0022: Enforce TLS server-certificate verification

**Status:** Accepted — signed off 2026-09-12; applied in the same PR that
introduces this ICP.
**Affected interfaces / docs:** No interface changes. `internal::ConnectOptions`
keeps its exact shape; no public or internal header is touched. What changes is
the *meaning* of three fields that were already there —
[`ca_bundle`](../../include/microtel/internal/transport.hpp), `insecure`, and
`sni_override` — in [`src/transport/http2_transport.cpp`](../../src/transport/http2_transport.cpp).
[`docs/error-model.md`](../error-model.md) gains no new `Error::Kind`; the new
failure rides `Kind::Network`.
**Affected tracks:** Track D (`src/transport/`).

## Summary

The TLS client never asked OpenSSL to verify the server's certificate. Turn
verification on (`SSL_VERIFY_PEER` plus hostname checking), skip it entirely
when `insecure == true`, and verify the same name that is sent in SNI.

## Motivation

`Http2Transport::TlsHandshake` loads `ca_bundle` into the `SSL_CTX` (or falls
back to the system trust store), sets the SNI extension, and then completes the
handshake. It never calls `SSL_CTX_set_verify`, never sets a name to check the
certificate against, and never reads `SSL_get_verify_result`.

An OpenSSL client defaults to `SSL_VERIFY_NONE`. With no verify mode set, the
loaded trust store is never consulted: the handshake completes against **any**
certificate, self-signed or not, issued to any name. A configured `ca_bundle`
looked like it was doing something and was doing nothing at all, which is worse
than an obviously-plaintext connection — an operator who sets `ca_bundle`
believes they have an authenticated channel. Every OTLP export over `https://`
has been trivially interceptable since the transport landed, and telemetry
carries exactly the sort of thing that should not be handed to a machine in the
middle: hostnames, request paths, user ids in span attributes, and whatever
bearer token the configured headers carry.

This is a bug fix, not an interface change, so it would not normally need an
ICP. It gets one because it flips observable behaviour for a configuration that
works today (see Migration), and because the name-selection rule below is a
decision someone will otherwise have to re-derive from the code.

## Proposed change

Three edits, all in `src/transport/http2_transport.cpp`:

1. **`LoadSslCtxCredentials`** — after the trust store is loaded, and only when
   `!opts.insecure`, call `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr)`.
   No callback: the default one is what we want, and a custom callback is the
   usual way this check gets quietly neutered again.

2. **`TlsHandshake`** — at the existing SNI site, and only when
   `!opts.insecure`, call `SSL_set1_host(ssl, verify_name.c_str())`.
   `SSL_set1_host` (OpenSSL ≥ 1.1.0) makes the built-in hostname check part of
   chain verification, so a name mismatch fails `SSL_connect` rather than
   needing a separate post-handshake check that a later edit could drop.

3. **`SslConnectLoop`** — when the handshake fails for a reason other than
   `WANT_READ`/`WANT_WRITE`, consult `SSL_get_verify_result`. If it is not
   `X509_V_OK`, report `"TLS certificate verification failed: <reason>"` from
   `X509_verify_cert_error_string` instead of the undifferentiated
   `"TLS handshake failed"`. "Bad certificate" and "the peer hung up mid-ClientHello"
   are different operational problems and should not share a message.

### The name that gets verified

**`verify_name` is `sni_override` when it is set, and the endpoint host
otherwise — exactly the expression that already selects the SNI value.** The
two are deliberately the same string, and that is the substantive decision in
this ICP.

The alternative — always verify the endpoint host, and let `sni_override` move
only the SNI extension — would break the configuration `sni_override` exists
for. Connecting to a collector at `https://127.0.0.1:4317` whose certificate is
issued to `localhost` works today by setting `sni_override = "localhost"`; under
the "always verify the endpoint host" rule that connection would present the
right SNI, receive the right certificate, and then be rejected for not being
issued to `127.0.0.1`. Tying the verified name to the override keeps
`sni_override` meaning one coherent thing: *this is the name I believe I am
talking to.*

The cost is that `sni_override` is now security-relevant rather than a
transport-layer detail — pointing it at a name the operator does not control
weakens verification to that name. That is the correct trade: it is still an
explicit, named host, not the blanket bypass that `insecure` provides.

### Known limitation: IP-address endpoints

`SSL_set1_host` performs an RFC 6125 *name* check. Before OpenSSL 3.2 it does
not match an IP literal against a certificate's `iPAddress` SAN, so
`https://10.0.0.5:4317` fronted by an IP-SAN certificate now fails verification
on the OpenSSL 3.0 that CI and current LTS distributions ship. Such a
deployment was unverified before this change, so nothing that was secure
becomes insecure; the workaround is `sni_override` set to a DNS name the
certificate carries, or `insecure = true`. Calling
`X509_VERIFY_PARAM_set1_ip_asc` for endpoints that parse as IP literals is the
proper fix and is deliberately left out of this change to keep the security fix
small and reviewable.

## Migration

**A connection that succeeds today can fail after this change.** Specifically,
any `https://` endpoint whose certificate is self-signed, expired, issued by a
CA outside the configured `ca_bundle`/system trust store, or issued to a name
other than the one being connected to. The transport is pre-1.0 and this is the
behaviour the configuration always claimed, so it changes in place rather than
behind a flag.

Operators have three options, in order of preference: fix the certificate; point
`ca_bundle` at the CA that issued it (and `sni_override` at the name it carries,
if that differs from the endpoint host); or set `insecure = true` to keep the
old behaviour deliberately. `MICROTEL_FORBID_INSECURE_TLS=ON` already exists to
stop the third from being chosen by accident in a build that must not allow it.

The failure surfaces through paths that already exist. `Connect` returns
`Error{Kind::Network, "TLS certificate verification failed: <reason>"}`; both
codecs' `EnsureConnected` turn that into a `WireResult` with
`retryable = true`, so the exporter retries and — since a bad certificate does
not get better — exhausts its retry budget and drops the batch. The message
reaches `HealthSnapshot::last_error_message`, which is where an operator
diagnosing this will look. No new `DropReason` is introduced;
`DropReason::ConnectFailure` still has no increment site (ICP 0018, open
question 2) and wiring it up belongs with #134's counter work, not here.

## Rationale & alternatives

- **Verify, but only warn on failure** — rejected. A verification failure that
  does not fail the connection is a log line nobody reads, and it leaves the
  transport with exactly the property this ICP is fixing.
- **A custom `verify_callback` to classify errors precisely** — rejected.
  `SSL_get_verify_result` after a failed `SSL_connect` gives the same
  information for the cost of three lines, and a verify callback is the standard
  place where "temporarily" returning 1 turns verification back off for good.
- **Default to `insecure = true` so nothing breaks** — rejected outright. The
  default must be the safe one; an insecure default is how this got shipped.
- **Gate the new behaviour behind a new `ConnectOptions` field** — rejected. It
  would be an interface change for the sake of preserving a bug, and the field
  that means "I accept an unverified peer" already exists and is called
  `insecure`.
