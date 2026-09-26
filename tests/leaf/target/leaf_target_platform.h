/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * What the target test runner (leaf_target_test.c) needs from the machine it
 * runs on. Two implementations: leaf_target_hosted.c (a Linux process: the
 * host build, 32-bit i686, qemu-aarch64 user mode) and leaf_target_cortex_m.c
 * (bare-metal Cortex-M under qemu-system-arm, output and exit over Arm
 * semihosting).
 */

#ifndef MICROTEL_LEAF_TARGET_PLATFORM_H
#define MICROTEL_LEAF_TARGET_PLATFORM_H

#include <stdint.h>

/** Writes a NUL-terminated string to the test log. */
void leaf_target_puts(const char* text);

/** Ends the run: status 0 is a pass, anything else a failure. Never returns. */
void leaf_target_exit(int status);

/**
 * The lowest address the stack measurement may paint, given `anchor`, an
 * address in the measuring function's frame: the bottom of the stack region on
 * bare metal, a fixed window below `anchor` in a process.
 */
uintptr_t leaf_target_stack_floor(uintptr_t anchor);

#endif /* MICROTEL_LEAF_TARGET_PLATFORM_H */
