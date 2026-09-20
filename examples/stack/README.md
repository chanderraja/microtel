# `examples/stack/` — the shared observability stack

One container stack that every example under `examples/` exports to:

```
example (microtel)  --OTLP/gRPC :4317-->  OTel Collector  --OTLP-->  Tempo  <--  Grafana
```

It exists so that running an example *shows you something*. Start it once, run
any example, and the trace is in Grafana seconds later with no configuration.

```bash
examples/stack/up.sh          # start, wait for all three, print the endpoints
# ... build and run examples ...
examples/stack/down.sh        # stop and remove everything
```

Then open **<http://localhost:3000>**. There is no login — the stack enables
anonymous admin access deliberately (see [Security](#security)). The home page
is the provisioned **microtel — recent traces** dashboard; click any row for
the flame graph. **Explore → Tempo** is the same data with a query box.

---

## What runs

| Service | Image (pinned) | Why |
|---|---|---|
| `otel-collector` | `otel/opentelemetry-collector-contrib:0.160.0` | What the examples export to. Same tag the conformance gate and the bench harness use (`ci/scripts/conformance.sh`, `bench/versions.lock`), so examples talk to the receiver the test suite is validated against. |
| `tempo` | `grafana/tempo:2.10.8` | Single-binary trace store behind the collector. |
| `grafana` | `grafana/grafana:13.2.2` | Reads Tempo. Datasource and dashboard are provisioned at boot. |

Tags are pinned on purpose: an example that worked last month should work
today. Bumping one is a normal PR — re-run the verification below with it.

### Ports

| Host port | Service | Used by |
|---|---|---|
| `4317` | collector, OTLP/**gRPC** | **every example** — this is the default endpoint |
| `4318` | collector, OTLP/**HTTP** | nothing here, today — plaintext, so microtel cannot reach it; see [the h2c note](#why-grpc-and-not-http) |
| `13133` | collector `health_check` | `up.sh` readiness polling |
| `3200` | Tempo HTTP API | Grafana's datasource, and the verification `curl` below |
| `3000` | Grafana | you |

Tempo's own OTLP receiver also listens on 4317, but *inside* the compose
network only — it is not published, so it cannot collide with the collector's.

### Files

| File | What it is |
|---|---|
| `compose.yaml` | the three services, pinned, with the portability constraints commented inline |
| `collector-config.yaml` | receive OTLP on 4317/4318, batch, export to Tempo, log a line per batch |
| `tempo.yaml` | single-binary Tempo, tuned so a trace is *searchable* in about ten seconds instead of Tempo's default minutes |
| `grafana/provisioning/` | the Tempo datasource (uid `tempo`) and the dashboard provider |
| `grafana/dashboards/` | the starter dashboard — one TraceQL `{}` table |
| `compose-engine.sh` | engine + compose front-end detection, sourced by `up.sh` and `down.sh` |

---

## Container engine

Both `up.sh` and `down.sh` resolve the engine the same way the rest of the
repo does (`ci/scripts/conformance.sh`, `bench/driver/container.py`):

1. `MICROTEL_CONTAINER_ENGINE` if set (`podman` or `docker`), else
2. **`podman` if it is installed**, else
3. `docker`.

podman comes first because the project's reference dev host is Fedora. Then
the compose front-end is resolved separately, because "which engine" and "which
compose" are two different questions:

- **podman** → `podman-compose` if on `PATH`, else `podman compose`.
  (`podman compose` only shells out to `podman-compose` anyway, and calling it
  directly skips its "executing external compose provider" banner.)
- **docker** → `docker compose` (v2 plugin) if it works, else `docker-compose`.

Force the other engine on a host that has both:

```bash
MICROTEL_CONTAINER_ENGINE=docker examples/stack/up.sh
```

`compose.yaml` stays inside the intersection of what both front-ends do
reliably: no `healthcheck:` blocks (`up.sh` polls the health endpoints itself),
`depends_on` in short list form only, `:z` on every bind mount, and no named
volumes.

---

## Verifying it works

Every example prints the trace ID it emitted. That is the handle for checking
the whole path end to end:

```bash
$ ./build/examples/microtel_example_basic_trace
trace_id: 532267361510131fac037e635454ce60
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed

$ curl -s http://localhost:3200/api/traces/532267361510131fac037e635454ce60 | head -c 200
```

Timings measured on the reference host: **trace-by-ID resolves about a second**
after the example exits; **search** (the dashboard, and Explore) picks it up
**about ten seconds** after, which is Tempo cutting the block and the querier
re-reading its blocklist. If the dashboard is empty, wait for one refresh
before concluding anything is wrong.

Searching without a `start`/`end` defaults to a recent window:

```bash
curl -sG http://localhost:3200/api/search --data-urlencode 'q={}' --data-urlencode 'limit=5'
```

---

## Storage is ephemeral

There are no named volumes. Each container writes to its own writable layer,
so `down.sh` takes the collected traces, and Grafana's sqlite database, with
it. The next `up.sh` starts clean.

That is the right trade for a demo stack: it sidesteps every volume-ownership
problem rootless podman has with images that run as a non-root UID, and it
means a stale trace from last week never confuses a reader. If you want traces
to survive a restart, add a volume for `/var/tempo` — and expect to `chown` it
to `10001:10001` first.

---

## Security

`compose.yaml` sets `GF_AUTH_ANONYMOUS_ENABLED=true` with
`GF_AUTH_ANONYMOUS_ORG_ROLE=Admin` and disables the login form. A login screen
between a first-time reader and their first trace is friction with no security
value on a stack that binds to localhost and holds nothing but example spans.

**Do not copy that block anywhere reachable from a network you do not own.**
The collector is likewise plaintext on 4317 and 4318 with no authentication.
For what a real deployment does instead, see
[`docs/auth-callback-recipes.md`](../../docs/auth-callback-recipes.md) and the
transport-security ledger in
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §3.

---

## Why gRPC and not HTTP

microtel's transport is HTTP/2-only, so a plaintext `http://` endpoint means
**h2c with prior knowledge**. The collector's plaintext OTLP/HTTP receiver on
4318 serves HTTP/1.1 only, so it answers the HTTP/2 preface with an HTTP/1.1
response and nothing is ever delivered.

This is documented, not a bug:
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §4 and
issue #166. Examples therefore use **OTLP/gRPC on 4317** (h2c by definition,
unaffected), or OTLP/HTTP over **TLS**, where ALPN negotiates `h2` and the same
collector works fine. 4318 stays published so that a reader can point another
OTLP client at it, but no example uses it: serving TLS here would mean mounting
certificates into *this* collector, so
[`examples/tls/`](../tls/) brings its own collector on its own ports instead
and leaves this one alone.

---

## Troubleshooting

**`up.sh` says a service never became ready.** Look at the logs — the command
is printed with the error:

```bash
podman-compose -f examples/stack/compose.yaml -p microtel-stack logs
```

**Permission denied reading a config file (SELinux).** Every bind mount in
`compose.yaml` already carries `:z`, which relabels it for SELinux under
rootless podman on Fedora/RHEL and is accepted and ignored by docker. If you
add a mount, add `:z` to it too. A missing label looks like the container
exiting immediately with a permission error on a file that is plainly
world-readable on the host — `sudo ausearch -m avc -ts recent` confirms it.

**Port already in use.** 4317 is the usual one: a leftover conformance-gate
collector (`ci/scripts/conformance.sh` names its container
`microtel-conformance-<pid>`) or a bench sink. `podman ps` and remove it.

**The example reports `batches_failed` and a connection error.** The stack is
not up, or something else owns 4317. `curl -s http://localhost:13133` should
return the collector's health JSON.

**Traces reach the collector but not Grafana.** Split the path:
`podman-compose -f examples/stack/compose.yaml -p microtel-stack logs otel-collector`
shows a `debug` exporter line per batch, so a line there plus an empty Grafana
points at the collector → Tempo hop, not at the example.

**`down.sh` leaves containers behind** after an interrupted `up.sh`. Remove
them by project name:

```bash
podman ps -a --filter 'name=microtel-stack' -q | xargs -r podman rm -f
```
