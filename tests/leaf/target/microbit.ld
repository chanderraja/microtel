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
MIN_STACK_BYTES = 2048;

INCLUDE cortex_m.ld
