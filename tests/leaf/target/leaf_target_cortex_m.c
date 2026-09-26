/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The target runner's platform on a bare-metal Cortex-M under qemu-system-arm:
 * the vector table, the reset handler, a fault handler that reports the
 * faulting PC, and log output and exit over Arm semihosting (the host
 * debugger interface QEMU implements with -semihosting-config enable=on).
 *
 * No heap is set up for the runner itself. The linker script (cortex_m.ld)
 * provides `end` for newlib's _sbrk only because the upb backend with no
 * scratch buffer, which the golden vectors use, allocates its arena (design
 * §2.2); the nanopb runner never calls it.
 */

#include "leaf_target_platform.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Semihosting operations (Arm "Semihosting for AArch32 and AArch64" 2.0). */
#define SEMIHOST_SYS_WRITE0 0x04u
#define SEMIHOST_SYS_EXIT 0x18u
#define SEMIHOST_EXIT_OK 0x20026u    /* ADP_Stopped_ApplicationExit */
#define SEMIHOST_EXIT_ERROR 0x20023u /* ADP_Stopped_RunTimeErrorUnknown */

/* The exception frame the core stacks on entry: r0-r3, r12, lr, pc, xpsr. */
#define FRAME_PC_INDEX 6u
#define HEX_DIGITS 8u
#define NIBBLE_BITS 4u
#define NIBBLE_MASK 0xfu
#define DECIMAL_DIGIT_LIMIT 10u

/* Vector table slots used: initial SP, reset, NMI, HardFault, and on v7-M
 * MemManage, BusFault and UsageFault (all the same handler). */
#define VECTOR_COUNT 16u

/* From the linker script. */
extern uint32_t __StackTop;
extern uint32_t __StackLimit;
extern uint32_t __data_load;
extern uint32_t __data_start;
extern uint32_t __data_end;
extern uint32_t __bss_start;
extern uint32_t __bss_end;

int main(void);

static uint32_t semihost_call(uint32_t op, const void* arg)
{
    register uint32_t r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void leaf_target_puts(const char* text)
{
    (void)semihost_call(SEMIHOST_SYS_WRITE0, text);
}

void leaf_target_exit(int status)
{
    /* 32-bit SYS_EXIT takes the reason code itself, not a parameter block;
     * QEMU exits with status 0 for ApplicationExit and 1 for anything else. */
    const uintptr_t reason = status == 0 ? SEMIHOST_EXIT_OK : SEMIHOST_EXIT_ERROR;
    (void)semihost_call(SEMIHOST_SYS_EXIT, (const void*)reason);
    for (;;)
    {
    }
}

uintptr_t leaf_target_stack_floor(uintptr_t anchor)
{
    (void)anchor;
    return (uintptr_t)&__StackLimit;
}

static void put_hex(uint32_t value)
{
    char text[HEX_DIGITS + 1u];
    size_t i;
    for (i = 0; i < HEX_DIGITS; ++i)
    {
        const uint32_t nibble = (value >> (NIBBLE_BITS * (HEX_DIGITS - 1u - i))) & NIBBLE_MASK;
        text[i] = (char)(nibble < DECIMAL_DIGIT_LIMIT ? '0' + nibble
                                                      : 'a' + (nibble - DECIMAL_DIGIT_LIMIT));
    }
    text[HEX_DIGITS] = '\0';
    leaf_target_puts(text);
}

/* Called from fault_entry with the stacked exception frame. An unaligned
 * access on ARMv6-M (Cortex-M0 / M0+) ends here: the leaf must never do one. */
void leaf_target_fault(const uint32_t* frame);

void leaf_target_fault(const uint32_t* frame)
{
    leaf_target_puts("\nFAULT: hard fault at pc=0x");
    put_hex(frame[FRAME_PC_INDEX]);
    leaf_target_puts("\n");
    leaf_target_exit(1);
}

/* Picks the stack the fault was taken on and passes its frame on. Thumb-1
 * only, so it assembles for ARMv6-M as well as ARMv7-M. */
__attribute__((naked)) static void fault_entry(void)
{
    __asm__ volatile("movs r0, #4\n"
                     "mov r1, lr\n"
                     "tst r0, r1\n"
                     "beq 1f\n"
                     "mrs r0, psp\n"
                     "b 2f\n"
                     "1:\n"
                     "mrs r0, msp\n"
                     "2:\n"
                     "ldr r2, =leaf_target_fault\n"
                     "bx r2\n"
                     ".ltorg\n");
}

void leaf_target_reset(void);

void leaf_target_reset(void)
{
    const size_t data_bytes = (size_t)((uintptr_t)&__data_end - (uintptr_t)&__data_start);
    const size_t bss_bytes = (size_t)((uintptr_t)&__bss_end - (uintptr_t)&__bss_start);
    memcpy(&__data_start, &__data_load, data_bytes);
    memset(&__bss_start, 0, bss_bytes);
    leaf_target_exit(main());
}

__attribute__((section(".isr_vector"), used)) static const uintptr_t k_vectors[VECTOR_COUNT] = {
    (uintptr_t)&__StackTop,
    (uintptr_t)&leaf_target_reset,
    (uintptr_t)&fault_entry, /* NMI */
    (uintptr_t)&fault_entry, /* HardFault */
    (uintptr_t)&fault_entry, /* MemManage (v7-M) */
    (uintptr_t)&fault_entry, /* BusFault (v7-M) */
    (uintptr_t)&fault_entry, /* UsageFault (v7-M) */
};
