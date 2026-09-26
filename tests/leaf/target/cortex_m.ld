/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sections for the target runner on a bare-metal Cortex-M. The board script
 * (microbit.ld, mps2_an386.ld) defines the FLASH and RAM regions and
 * includes this file.
 *
 * RAM: .data, .bss, then the heap newlib's _sbrk grows up from `end` (used
 * only by the upb backend with no scratch buffer), then the stack, which
 * grows down from the top of RAM to __StackLimit. The stack measurement
 * paints [__StackLimit, SP).
 */

ENTRY(leaf_target_reset)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* libgcc's unwinder is linked in through newlib's abort(), which upb
     * references; it needs the (empty) index bounds. */
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
    __HeapLimit = end + HEAP_BYTES;
    __StackLimit = __HeapLimit;
    __StackTop = ORIGIN(RAM) + LENGTH(RAM);
    ASSERT(__StackTop - __StackLimit >= MIN_STACK_BYTES, "not enough RAM left for the stack")
}
