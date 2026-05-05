# `tests/wire/`

Protocol byte-level fixtures for the encoder and the OTLP/HTTP codec.
Each test fixture is a captured byte stream paired with the expected
parser outcome.

For the gRPC codec, see [`tests/grpc-wire/`](../grpc-wire/) — the
gRPC corpus is large enough to deserve its own directory.

## Suggested subdirectories

| Subdirectory | Theme |
|---|---|
| `encoder/` | OTLP encoder fixtures: hand-encoded `ExportTraceServiceRequest` payloads verified against canonical upstream encoders. |
| `http/`    | OTLP/HTTP codec: captured collector responses (200, partial-success body, 415, 429, 502/503/504, malformed). |

## Bar

- **Each fixture is byte-identical** across runs. The encoded payload
  is committed; CI does not regenerate it. (Matches the discipline
  established by the M1 spike fixture.)
- **One test per row** of the matrix in `docs/error-model.md` §7.1
  (HTTP) / §7.2 (gRPC, in `tests/grpc-wire/`).
- **No external dependencies.** Tests load fixtures from disk, run the
  parser, assert the outcome. No collector, no network.
