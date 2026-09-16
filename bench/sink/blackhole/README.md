# blackhole-sink

A zero-logic OTLP receiver used as the benchmarking target for microtel.
It accepts spans over OTLP/gRPC and OTLP/HTTP, counts them with atomic
counters, and discards everything else.  Latency and throughput reflect the
microtel SDK and network path, not any collector work.

## Ports

| Port | Protocol | Endpoint |
|------|----------|----------|
| 4317 | gRPC (plaintext) | OTLP TraceService.Export |
| 4318 | HTTP/2 cleartext (h2c) | POST /v1/traces, /v1/metrics, /v1/logs |
| 19080 | HTTP/1.1 | GET /health, GET /stats, POST /reset |

## Control API

```
GET  /health   → 200 "ok"
GET  /stats    → 200 application/json (see Snapshot fields below)
POST /reset    → 200 "{}" (zeroes all counters; uptime is not reset)
```

### Snapshot fields

| Field | Type | Description |
|-------|------|-------------|
| `spans_received` | uint64 | Total span records counted |
| `bytes_received` | uint64 | Wire bytes in (HTTP: body size; gRPC: proto.Size — see Compression) |
| `requests_received` | uint64 | HTTP + gRPC request total |
| `http_requests_received` | uint64 | HTTP-only request count |
| `grpc_requests_received` | uint64 | gRPC-only request count |
| `response_bytes` | uint64 | Total serialized response bytes sent |
| `errors` | uint64 | Requests rejected with an error |
| `last_error` | string | Description of most recent error, empty if none |
| `uptime_seconds` | float64 | Seconds since process start (not reset by /reset) |

## Compression

Both listeners inflate gzip before counting spans, so the `*-gzip` SUTs report
real delivery:

| Path | Trigger | Inflated by |
|------|---------|-------------|
| HTTP | `content-encoding: gzip` | the trace handler, after `bytes_received` is taken |
| gRPC | `grpc-encoding: gzip` (message CF=`0x01`) | grpc-go, via the registered gzip compressor |

`bytes_received` means the same thing on both: the compressed size of what
arrived. grpc-go inflates before the handler runs, so the gRPC path cannot
measure the request inside the handler — `StatsHandlerOption()` installs a
`grpc.StatsHandler` that records `stats.InPayload.WireLength` per RPC and the
handler reads that instead of `proto.Size` ([#228](https://github.com/chanderraja/microtel/issues/228)).
`WireLength` includes the 5-byte gRPC length-prefix header, so uncompressed
gRPC byte baselines are 5 bytes per message higher than before that landed.

Any server registering these handlers must pass `StatsHandlerOption()`;
without it `bytes_received` silently falls back to the uncompressed
`proto.Size`.

## Run with Docker

```bash
docker build -t blackhole-sink .
docker run --rm -p 4317:4317 -p 4318:4318 -p 19080:19080 blackhole-sink
```

## Run locally

```bash
go run ./cmd/blackhole-sink
```

Requires Go 1.22+.

## gRPC reflection

The gRPC server registers the standard server reflection service, so you can
inspect it with `grpcurl`:

```bash
grpcurl -plaintext localhost:4317 list
grpcurl -plaintext localhost:4317 describe opentelemetry.proto.collector.trace.v1.TraceService
```
