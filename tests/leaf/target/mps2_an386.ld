/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Arm MPS2 with the AN386 FPGA image (Cortex-M4) as QEMU models it
 * (-M mps2-an386): 4 MiB SSRAM1 at 0 for code, SSRAM2/3 at 0x20000000 for
 * data (1 MiB of it used here). 64 KiB of heap for the upb backend's arena
 * when no scratch buffer is given (the golden vectors).
 */

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 1M
}

HEAP_BYTES = 65536;
MIN_STACK_BYTES = 65536;

INCLUDE cortex_m.ld
