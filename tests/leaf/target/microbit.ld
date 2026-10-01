/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * BBC micro:bit (nRF51822, Cortex-M0) as QEMU models it (-M microbit):
 * 256 KiB flash at 0, 16 KiB SRAM at 0x20000000. The nanopb runner only, so
 * no heap.
 */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}

HEAP_BYTES = 0;
/* The deepest entry point, the encode, needs 3,552 bytes by the static call
 * graph (docs/bench-results/leaf-footprint.md), and the runner's own frames
 * sit above it; the runner fails if a test reaches the bottom of the stack. */
MIN_STACK_BYTES = 4608;

INCLUDE cortex_m.ld
