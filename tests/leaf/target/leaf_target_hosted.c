/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/* The target runner's platform in a process: stdout and exit(). */

#include "leaf_target_platform.h"

#include <stdio.h>
#include <stdlib.h>

/* How far below the measuring frame the stack is painted. A process's main
 * stack is far larger; this is only what the deepest entry point needs, with
 * room to spare. */
#define HOSTED_STACK_WINDOW_BYTES 65536u

void leaf_target_puts(const char* text)
{
    (void)fputs(text, stdout);
}

void leaf_target_exit(int status)
{
    (void)fflush(stdout);
    exit(status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

uintptr_t leaf_target_stack_floor(uintptr_t anchor)
{
    return anchor - HOSTED_STACK_WINDOW_BYTES;
}
