/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board layer of the demo firmware: Arm MPS2 + AN386 (Cortex-M4) as QEMU
 * models it (-M mps2-an386). A millisecond tick from SysTick and a polled
 * CMSDK APB UART0 — the only "hardware" the firmware touches.
 */
#ifndef EDGE_DEMO_BOARD_H
#define EDGE_DEMO_BOARD_H

#include <stddef.h>
#include <stdint.h>

/** Monotonic nanoseconds since reset (SysTick, 1 ms resolution + sub-tick). */
uint64_t board_now_ns(void);
/** Busy-waits `ms` milliseconds. */
void board_delay_ms(uint32_t ms);
/** Writes a NUL-terminated string to UART0. */
void board_uart_puts(const char* text);
/** Writes one byte to UART0. */
void board_uart_putc(char c);
/** Reads one line (without the newline) from UART0, blocking. Returns its length. */
size_t board_uart_getline(char* out, size_t out_size);

#endif
