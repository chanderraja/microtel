/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The golden vectors (the .bin files in tests/leaf/vectors/) compiled into the target
 * runner, which has no file system. The definitions are generated at
 * configure time by leaf_target.cmake from the committed files.
 */

#ifndef MICROTEL_LEAF_TARGET_GOLDEN_H
#define MICROTEL_LEAF_TARGET_GOLDEN_H

#include <stddef.h>
#include <stdint.h>

/** One committed golden payload. */
typedef struct leaf_target_golden
{
    const char* name; /**< the vector name, the .bin file's stem */
    const uint8_t* bytes;
    size_t size;
} leaf_target_golden;

extern const leaf_target_golden leaf_target_goldens[];
extern const size_t leaf_target_golden_count;

#endif /* MICROTEL_LEAF_TARGET_GOLDEN_H */
