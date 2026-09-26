# Leaf footprint

Flash and RAM of the experimental microtel leaf, published for both encoder
backends as ICP 0031 gate 4 requires (design:
[`leaf-concentrator-design.md`](../leaf-concentrator-design.md) §7.6). The
`leaf-footprint` CI job produces the same tables on every PR, in its job
summary; this file records them for the release.

**v1.2 figures**, measured 2026-09-26 at the commit that added the job, with
`ci/scripts/leaf-footprint.sh` in an `ubuntu:24.04` container using the same apt
toolchains as CI: `arm-none-eabi-gcc` 13.2.1 (newlib-nano) and
`aarch64-linux-gnu-gcc` 13.3.0.

## What is measured

[`examples/leaf/size_probe.c`](../../examples/leaf/size_probe.c) is a minimal
trace-only firmware: it initialises a leaf in static memory, builds one span
with one attribute, and streams the payload to a sink. The leaf is built
standalone (`cmake -S leaf`, `MinSizeRel`, so `-Os`, with
`-ffunction-sections -fdata-sections`) and the probe is linked with
`--gc-sections`. The tables count the input sections from the leaf's own
archives that survive the link: the leaf core, its backend, and the vendored
encoder's runtime and generated tables. libc and the probe itself are not
counted.

Flash is `.text` + `.rodata` + `.data`. The targets are ICP 0031's: under
15 KB for nanopb on a Cortex-M, under 30 KB for upb on a Cortex-A / Linux-on-ARM
target. **They are targets in v1.2, not gates**; v2.1 makes the nanopb one a
gate.

## nanopb, Cortex-M0+ (`-mcpu=cortex-m0plus -mthumb`)

| part | .text | .rodata | .data | .bss |
|---|---:|---:|---:|---:|
| leaf core | 5,074 | 186 | 0 | 0 |
| leaf backend (nanopb) | 1,086 | 0 | 0 | 0 |
| nanopb runtime | 2,206 | 0 | 0 | 0 |
| nanopb descriptors | 0 | 920 | 0 | 0 |
| **total** | **8,366** | **1,106** | **0** | **0** |

**Flash 9,472 bytes: under the 15 KB target.** Whole probe image including
newlib-nano start-up: text 11,436, data 12, bss 1,012.

## nanopb, Cortex-M4 (`-mcpu=cortex-m4 -mthumb`)

| part | .text | .rodata | .data | .bss |
|---|---:|---:|---:|---:|
| leaf core | 4,962 | 186 | 0 | 0 |
| leaf backend (nanopb) | 1,100 | 0 | 0 | 0 |
| nanopb runtime | 2,130 | 0 | 0 | 0 |
| nanopb descriptors | 0 | 920 | 0 | 0 |
| **total** | **8,192** | **1,106** | **0** | **0** |

**Flash 9,298 bytes: under the 15 KB target.** Whole probe image: text 11,344,
data 12, bss 1,012.

## upb, aarch64 Linux

| part | .text | .rodata | .data | .bss |
|---|---:|---:|---:|---:|
| leaf core | 8,800 | 186 | 0 | 0 |
| leaf backend (upb) | 4,192 | 18 | 8 | 0 |
| upb runtime | 9,144 | 150 | 168 | 1 |
| upb mini-tables | 0 | 708 | 592 | 0 |
| **total** | **22,136** | **1,062** | **768** | **1** |

**Flash 23,966 bytes: under the 30 KB target.** Unwind tables (`.eh_frame`,
3,744 bytes, on by default for Linux targets) are not counted; build with
`-fno-asynchronous-unwind-tables` to drop them.

## RAM

The leaf has no static state of its own on nanopb (0 bytes `.data` + `.bss`)
and 769 bytes on upb (the runtime's tables). Everything else is memory the
caller owns and sizes:

| Caller-owned | Bytes | Notes |
|---|---:|---|
| `microtel_leaf_t` | 256 | fixed, both backends |
| record buffer | 256 | what the probe's Resource, scope and one span with one attribute need (a little over 192) |
| upb encode scratch | 2,048 | upb only; the probe's payload needs a little over 1.25 KiB on a 64-bit target. nanopb needs none |
| output buffer | 0 | the probe streams with `microtel_leaf_encode_to`; a buffer encode needs the payload size (204 bytes here) |

## Closure

The same job runs `ci/scripts/symbol-scan.sh` over the installed archives with
the target's `nm`: no C++ runtime symbol, every global prefixed `microtel_`,
and for nanopb no reference to `malloc`, `calloc`, `realloc`, `free`,
`aligned_alloc` or `posix_memalign`. All three targets are clean.

## Reproducing

```bash
ci/scripts/leaf-footprint.sh cortex-m0plus   # or cortex-m4, aarch64
```

It needs `arm-none-eabi-gcc` with newlib (apt `gcc-arm-none-eabi
libnewlib-arm-none-eabi`) or `aarch64-linux-gnu-gcc` (apt
`gcc-aarch64-linux-gnu`), CMake, Ninja and Python 3. Other compiler versions
give somewhat different numbers.
