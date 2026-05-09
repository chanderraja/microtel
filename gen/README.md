# gen/ — Generated upb C accessors

This directory contains the upb-generated C accessor files for microtel's
four OTLP proto schemas. The files are committed here so a plain clone
has everything needed to build — no code-generation step required at
configure time.

## What's here

Generated from `proto/opentelemetry/proto/{common,resource,trace,collector/trace}/v1/*.proto`
using `protoc-gen-upb` and `protoc-gen-upb_minitable`:

```
opentelemetry/proto/common/v1/
  common.upb.h          — field accessor declarations (inlined)
  common.upb.c          — (empty in upb v29.4+ — accessors are header-only)
  common.upb_minitable.h — mini-table declarations
  common.upb_minitable.c — mini-table definitions (the actual data)

opentelemetry/proto/resource/v1/   (same four files)
opentelemetry/proto/trace/v1/      (same four files)
opentelemetry/proto/collector/trace/v1/  (same four files)
```

The empty `.upb.c` files are expected — upb v29.4 inlines all accessor
implementations into the `.upb.h` header, so the `.c` file has no body.

## Regenerating

Run `ci/scripts/regen-protos.sh` after bumping the `proto/` pin:

```bash
ci/scripts/regen-protos.sh \
  --protoc       /path/to/protoc-29.4 \
  --gen-upb      /path/to/protoc-gen-upb-29.4.0 \
  --gen-upb-mt   /path/to/protoc-gen-upb_minitable-29.4.0
```

The script documents how to build the two plugins from the pinned
protobuf v29.4 source. After running it, `git diff gen/` shows exactly
what changed.

## CI gate

The `regen-check` job in `.github/workflows/ci.yml` runs
`ci/scripts/regen-protos.sh` and asserts `git diff --exit-code gen/`
after regeneration. A PR that changes `proto/` without updating `gen/`
fails CI.
