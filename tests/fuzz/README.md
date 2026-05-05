# `tests/fuzz/`

libFuzzer harnesses for adversarial inputs. Required for v1.0 release
per `microtel-spec.md` §13.5:

> Fuzzing (gRPC framing/trailer paths, TOML parser, response-size
> limits), soak tests, perf gates in CI, collector interop matrix CI...

## Required harnesses (per spec §13.5 / §14.2 / §16)

| File | Surface |
|---|---|
| `grpc_codec_fuzz.cpp`         | The gRPC response parser entry point. Validates split-prefix, multi-frame, trailer parsing, RetryInfo decoding. |
| `toml_fuzz.cpp`               | The `microtel.toml` parser. Adversarial TOML inputs. |
| `response_decompression_fuzz.cpp` | Decompression-bomb protection; bounded by `max_decompressed_bytes`. |
| `otlp_response_fuzz.cpp`      | The `ExportTraceServiceResponse` proto parser (partial-success path). |

## Invariants

Per `docs/grpc-wire-protocol.md` §7.4 — and applicable to every fuzz
harness:

- **No crashes.**
- **No ASAN / UBSAN / TSAN findings.**
- **Memory growth is bounded** by the configured response-size limits
  regardless of input.
- **No infinite loops.** Parser always makes progress or terminates.

## CI

Fuzzing runs as a periodic CI job, not a per-PR gate (it's slow and
noisy). Mutation testing follows the same pattern (per spec §14.2).
Findings open issues for follow-up; they do not block PRs.

## Reproduction

Each crashing input is committed under a per-harness `crashes/`
subdirectory once a finding is confirmed. The harness re-runs against
its `crashes/` corpus on every PR — that part *is* a hard gate. Once
fixed, the input stays in the corpus as a regression check.
