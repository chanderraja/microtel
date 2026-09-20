# `tls/` — TLS, a custom CA, mTLS, and the one place OTLP/HTTP works

```cpp
builder.WithEndpoint("https://localhost:5327")
    .WithProtocol(microtel::Protocol::Grpc)
    .WithTls({.insecure = false,
              .ca_bundle = "examples/tls/certs/ca.crt",
              .client_cert = {},        // mTLS: the client's certificate
              .client_key = {},         // mTLS: its key
              .sni_override = {}});     // empty: derived from the endpoint host
```

An empty `ca_bundle` means the **system trust store**, not "trust anything".
`insecure = true` is the only way to skip verification, it warns at `Build()`,
and `MICROTEL_FORBID_INSECURE_TLS=ON` turns it into a build failure.

---

## Running it

The example needs a TLS receiver and a CA to pin, so it brings both — an
**opt-in overlay**, not a change to the shared stack.

```bash
examples/stack/up.sh          # the shared stack, so accepted traces reach Grafana
examples/tls/up-tls.sh        # generates certs, then starts the TLS collector

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_tls      # from the repository root

examples/tls/down-tls.sh      # tears down only the overlay
```

Arguments, all optional:
`[grpc-endpoint] [http-endpoint] [mtls-endpoint] [cert-dir]`, defaulting to
`https://localhost:5327`, `https://localhost:5328`, `https://localhost:5337`
and `examples/tls/certs`. Run it from the repository root, or pass the cert
directory.

### What the overlay is

| File | What it is |
|---|---|
| `gen-certs.sh` | a throwaway CA, a server certificate for `localhost`, and a client certificate for mTLS |
| `tls-collector-config.yaml` | three receivers — TLS gRPC (5327), TLS HTTP (5328), mTLS gRPC (5337) — forwarding what they accept to the shared stack's collector |
| `tls-compose.yaml` | that collector as its own compose project, `microtel-tls-example` |
| `up-tls.sh` / `down-tls.sh` | start and stop it, using the shared stack's engine detection (`../stack/compose-engine.sh`) |

**Nothing in `examples/stack/` is touched.** Separate project, separate ports
(5327/5328/5337, and 15134 for health), so every other example keeps exporting
to 4317 while this runs. `certs/` is gitignored; `up-tls.sh` generates it if it
is missing, and `MICROTEL_CERT_FORCE=1` regenerates it.

### The certificates

`gen-certs.sh` makes P-256 keys, matching `ci/scripts/conformance.sh`, so the
example exercises the shape of material the conformance gate does. The server
certificate carries `CN=localhost` **and**
`subjectAltName = DNS:localhost, IP:127.0.0.1`. The SAN is load-bearing:
microtel verifies the hostname (ICP [0022](../../docs/icps/0022-tls-peer-verification.md)),
and a certificate with only a CN is rejected. Everything is `chmod 0644` —
the collector reads it as uid 10001, and a key the container cannot open is
the most common way this example fails to start.

None of it is secret. It authenticates a demo collector on localhost.

---

## The four phases

| Phase | Configuration | Outcome |
|---|---|---|
| 1 | gRPC, **no** `ca_bundle` | `Connect()` fails: `TLS certificate verification failed: unable to get local issuer certificate` |
| 2 | gRPC, `ca_bundle` pinned | connects, `batches_sent=1` |
| 3 | **HTTP**, `ca_bundle` pinned | connects, `batches_sent=1` |
| 4 | gRPC, `ca_bundle` + client certificate, against the mTLS port | connects, `batches_sent=1` |

### Sample run

```
=== phase 1: gRPC over TLS, no ca_bundle (expected to fail) ===
  endpoint: https://localhost:5327
  ca_bundle: (system trust)
  Connect() failed: TLS certificate verification failed: unable to get local issuer certificate
  ForceFlush: Completed
  health: connection_state=Disconnected batches_sent=0 batches_failed=1 ConnectFailure=5
  last_error: TLS certificate verification failed: unable to get local issuer certificate

=== phase 2: gRPC over TLS, ca_bundle pinned ===
  Connect(): ok
  trace_id: 445f7a8ae25442693518191f30f5fba3
  health: connection_state=Connected batches_sent=1 batches_failed=0 ConnectFailure=0

=== phase 3: OTLP/HTTP over TLS, ca_bundle pinned ===
  endpoint: https://localhost:5328
  Connect(): ok
  trace_id: fc1b795e0dfed01b2f42d2fd40e128f1
  health: connection_state=Connected batches_sent=1 batches_failed=0 ConnectFailure=0

=== phase 4: gRPC over mTLS, client certificate presented ===
  endpoint: https://localhost:5337
  Connect(): ok
  trace_id: e4ccd18543e15497e147f66819f66946
  health: connection_state=Connected batches_sent=1 batches_failed=0 ConnectFailure=0
```

---

## The failure without `ca_bundle`

Phase 1 is the only kind of TLS assertion that can catch verification silently
not happening: **a client that accepts anything passes every positive phase in
this file.** The overlay's CA is self-signed and in no trust store on the
machine, so with `ca_bundle` empty the peer is verified against system trust
and refused —

```
TLS certificate verification failed: unable to get local issuer certificate
```

— at `Connect()`, before any span is on the wire. The same message reaches
`HealthSnapshot::last_error_message`, and `ConnectFailure` counts each attempt.

That this test *can* fail is not hypothetical: before ICP 0022, `ca_bundle` was
loaded into a trust store that `SSL_VERIFY_NONE` never consulted, and a client
in that state connects happily to a server its configured CA does not vouch
for.

The three ways to make phase 1 succeed, in descending order of sanity: pin the
CA (phase 2); install the CA in the system trust store; set `insecure = true`
and accept that you have turned verification off.

---

## OTLP/HTTP works here and nowhere else

Phase 3 is the only configuration in which microtel can use OTLP/HTTP against a
stock collector. microtel's transport is HTTP/2-only, so a plaintext `http://`
endpoint means **h2c with prior knowledge** — and the collector's plaintext
OTLP/HTTP receiver serves HTTP/1.1 only, answering the HTTP/2 preface with an
HTTP/1.1 response. Over TLS, ALPN negotiates `h2` and the same receiver works.

microtel says so in three places rather than failing silently: a `Build()`
warning, a `Connect()` error of kind `Protocol`, and
`last_error_message`. Full detail in
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §4 and
issue #166.

---

## The TLS 1.2 floor

microtel calls `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)` itself rather
than inheriting the linked OpenSSL's floor, so **a receiver limited to TLS 1.0
or 1.1 fails the handshake on every build alike, and there is no option to
lower it** (`docs/compatibility-matrix.md` §3, issue #216). TLS 1.3 is used
where the receiver offers it — which the pinned collector does, and which is
why the mTLS rejection below looks the way it does.

---

## mTLS, and the variant that fails

Phase 4 presents `client.crt` / `client.key` to the receiver on 5337, which is
configured with `client_ca_file`. The collector config supports it cleanly, so
it is a phase rather than a footnote; it is skipped automatically if the client
material is missing.

The negative case — the mTLS port **without** a client certificate — is one
argument away:

```bash
./build/examples/microtel_example_tls https://localhost:5337 https://localhost:5328 https://localhost:5337
```

Phase 2 then dials the mTLS port with the CA pinned but no client certificate,
and reports:

```
  Connect() failed: nghttp2 recv failed during SETTINGS exchange
  health: connection_state=Disconnected batches_sent=0 batches_failed=1 ConnectFailure=5
```

**That message is not about certificates, and that is worth understanding
rather than papering over.** Under TLS 1.3 the client finishes its side of the
handshake before the server has validated the client certificate, so the
server's refusal arrives afterwards as the connection going away — which
microtel reports at the layer where it noticed, the HTTP/2 SETTINGS exchange.
The connection is refused either way; only the wording differs from phase 1's,
where verification failed on the client's own side and microtel could name the
reason precisely. When mTLS is misconfigured, the collector's log is the place
that says so plainly:

```bash
podman-compose -f examples/tls/tls-compose.yaml -p microtel-tls-example logs
```

---

## Where this is validated properly

The example demonstrates; `tests/conformance/{http,grpc}/tls_test.cpp` asserts
— `TlsCustomCa`, `UntrustedCaFails`, `MutualTls`,
`MutualTlsWithoutClientCertFails`, `SniOverride` — against a collector started
by `ci/scripts/conformance.sh`. The transport-security ledger, including what
is **unsupported** (TLS 1.0/1.1 receivers, HTTP proxies, in-process SigV4), is
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §3.
