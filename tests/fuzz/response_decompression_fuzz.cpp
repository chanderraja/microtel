// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for response decompression bomb protection.
//
// DEFERRED: gzip/deflate response decompression (grpc-encoding: gzip,
// content-encoding: gzip) is not yet implemented. This placeholder ensures
// the build infrastructure is in place and the harness appears in the
// required-harness list in tests/fuzz/README.md.
//
// Fill in the harness body when decompression support lands (targeted for
// a post-M5 patch). The invariant to enforce: decompressed output is always
// bounded by max_decompressed_bytes regardless of the compression ratio of
// the input.

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* /*data*/, size_t /*size*/)
{
    return 0;
}
