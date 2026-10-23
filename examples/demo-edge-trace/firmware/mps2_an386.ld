/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPS2 AN386 as QEMU models it: 4 MiB SSRAM1 at 0 for code, RAM at
 * 0x20000000. The firmware uses 64 KiB of RAM and no heap: the leaf's record
 * buffer and the upb encode arena are static arrays.
 */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 4M
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 64K
}

ENTRY(board_reset)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    __data_load = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    end = __bss_end;
    __StackTop = ORIGIN(RAM) + LENGTH(RAM);
    ASSERT(__StackTop - __bss_end >= 16K, "not enough RAM left for the stack")
}
