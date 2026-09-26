# gen/ — Generated upb C accessors

The upb-generated C accessors for the OTLP schemas vendored in
[`proto/`](../proto/README.md). They are committed so that a plain clone
builds without a code-generation step at configure time.

## What's here

One set of four files per schema, generated from `proto/` with
`protoc-gen-upb` and `protoc-gen-upb_minitable`:

```
opentelemetry/proto/common/v1/
  common.upb.h            field accessor declarations (inlined)
  common.upb.c            empty; accessors are header-only since upb v29.4
  common.upb_minitable.h  mini-table declarations
  common.upb_minitable.c  mini-table definitions (the actual data)

opentelemetry/proto/resource/v1/            (same four files)
opentelemetry/proto/trace/v1/               (same four files)
opentelemetry/proto/collector/trace/v1/     (same four files)
opentelemetry/proto/metrics/v1/             (same four files)
opentelemetry/proto/collector/metrics/v1/   (same four files)
opentelemetry/proto/logs/v1/                (same four files)
opentelemetry/proto/collector/logs/v1/      (same four files)
```

The `.upb.c` files are supposed to be empty. upb v29.4 inlines every accessor
into the `.upb.h` header, which leaves the `.c` file with no body.

## Regenerating

Run `ci/scripts/regen-protos.sh` after bumping the `proto/` pin:

```bash
ci/scripts/regen-protos.sh \
  --protoc       /path/to/protoc-29.4 \
  --gen-upb      /path/to/protoc-gen-upb-29.4.0 \
  --gen-upb-mt   /path/to/protoc-gen-upb_minitable-29.4.0
```

The script explains how to build the two plugins from the pinned protobuf
v29.4 source. Afterwards, `git diff gen/` shows what changed.

## CI gate

The `regen-check` job in `.github/workflows/ci.yml` runs
`ci/scripts/regen-protos.sh` and then `git diff --exit-code gen/`, so a PR
that changes `proto/` without updating `gen/` fails CI.

## nanopb/ — the leaf's descriptors

[`nanopb/`](nanopb/) holds nanopb output for the four trace-path schemas
(common, resource, trace, collector/trace), used only by the leaf's nanopb
encoder backend (`docs/leaf-concentrator-design.md` §2.4). It is a sibling
tree, so the upb output above is unaffected. The generator options are in
[`leaf/nanopb/otlp_trace.options`](../leaf/nanopb/otlp_trace.options). Pass
`--nanopb-generator` to `regen-protos.sh` to regenerate it; the script's
header gives the pinned generator version and install steps. `regen-check`
passes the flag, so this tree is held to the same zero-diff rule.
