/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal startup and drivers for the MPS2 AN386 under qemu-system-arm:
 * vector table, reset handler (copies .data, zeroes .bss), SysTick at 1 kHz
 * from the 25 MHz system clock, and polled CMSDK APB UART0 at 0x40004000,
 * which QEMU connects to its first -serial backend.
 */
#include "board.h"

#include <string.h>

#define SYSCLK_HZ 25000000u
#define TICK_HZ 1000u
#define TICK_RELOAD (SYSCLK_HZ / TICK_HZ - 1u)
#define NS_PER_MS 1000000ull
#define NS_PER_CYCLE 40u /* 1 / 25 MHz */

#define REG32(addr) (*(volatile uint32_t*)(addr))

/* SysTick (Armv7-M ARM B3.3). */
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)
#define SYST_CSR_ENABLE 0x1u
#define SYST_CSR_TICKINT 0x2u
#define SYST_CSR_CLKSOURCE 0x4u

/* CMSDK APB UART (Arm DDI 0479). */
#define UART0_BASE 0x40004000u
#define UART_DATA REG32(UART0_BASE + 0x00u)
#define UART_STATE REG32(UART0_BASE + 0x04u)
#define UART_CTRL REG32(UART0_BASE + 0x08u)
#define UART_BAUDDIV REG32(UART0_BASE + 0x10u)
#define UART_STATE_TXFULL 0x1u
#define UART_STATE_RXFULL 0x2u
#define UART_CTRL_TXEN 0x1u
#define UART_CTRL_RXEN 0x2u
#define UART_BAUDDIV_115200 (SYSCLK_HZ / 115200u)

#define VECTOR_COUNT 16u
#define SYSTICK_VECTOR 15u

extern uint32_t __StackTop;
extern uint32_t __data_load;
extern uint32_t __data_start;
extern uint32_t __data_end;
extern uint32_t __bss_start;
extern uint32_t __bss_end;

int main(void);

static volatile uint32_t g_ms;

void board_systick_handler(void);
void board_systick_handler(void)
{
    g_ms = g_ms + 1u;
}

uint64_t board_now_ns(void)
{
    uint32_t ms;
    uint32_t cvr;
    do
    {
        ms = g_ms;
        cvr = SYST_CVR;
    } while (ms != g_ms);
    return (uint64_t)ms * NS_PER_MS + (uint64_t)(TICK_RELOAD - cvr) * NS_PER_CYCLE;
}

void board_delay_ms(uint32_t ms)
{
    const uint32_t start = g_ms;
    while ((uint32_t)(g_ms - start) < ms)
    {
    }
}

void board_uart_putc(char c)
{
    while ((UART_STATE & UART_STATE_TXFULL) != 0u)
    {
    }
    UART_DATA = (uint32_t)(uint8_t)c;
}

void board_uart_puts(const char* text)
{
    while (*text != '\0')
    {
        board_uart_putc(*text++);
    }
}

size_t board_uart_getline(char* out, size_t out_size)
{
    size_t n = 0;
    for (;;)
    {
        char c;
        while ((UART_STATE & UART_STATE_RXFULL) == 0u)
        {
        }
        c = (char)(UART_DATA & 0xffu);
        if (c == '\r')
        {
            continue;
        }
        if (c == '\n')
        {
            break;
        }
        if (n + 1u < out_size)
        {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return n;
}

static void fault_handler(void)
{
    board_uart_puts("LOG FAULT: hard fault\n");
    for (;;)
    {
    }
}

void board_reset(void);
void board_reset(void)
{
    const size_t data_bytes = (size_t)((uintptr_t)&__data_end - (uintptr_t)&__data_start);
    const size_t bss_bytes = (size_t)((uintptr_t)&__bss_end - (uintptr_t)&__bss_start);
    memcpy(&__data_start, &__data_load, data_bytes);
    memset(&__bss_start, 0, bss_bytes);

    UART_BAUDDIV = UART_BAUDDIV_115200;
    UART_CTRL = UART_CTRL_TXEN | UART_CTRL_RXEN;

    SYST_RVR = TICK_RELOAD;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    (void)main();
    for (;;)
    {
    }
}

__attribute__((section(".isr_vector"), used)) static const uintptr_t k_vectors[VECTOR_COUNT] = {
    [0] = (uintptr_t)&__StackTop,
    [1] = (uintptr_t)&board_reset,
    [2] = (uintptr_t)&fault_handler, /* NMI */
    [3] = (uintptr_t)&fault_handler, /* HardFault */
    [4] = (uintptr_t)&fault_handler, /* MemManage */
    [5] = (uintptr_t)&fault_handler, /* BusFault */
    [6] = (uintptr_t)&fault_handler, /* UsageFault */
    [SYSTICK_VECTOR] = (uintptr_t)&board_systick_handler,
};
