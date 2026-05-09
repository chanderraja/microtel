// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SslCtx. Verifies move-only semantics, Release(), Reset(), and
// that the destructor calls SSL_CTX_free — ASan catches double-free if it does not.

#include "common/raii/ssl_ctx.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

namespace mcr = microtel::common::raii;

static mcr::SslCtx MakeCtx()
{
    return mcr::SslCtx{::SSL_CTX_new(::TLS_method())};
}

TEST(SslCtxTest, DefaultConstruct_NotValid)
{
    const mcr::SslCtx ctx;
    EXPECT_FALSE(ctx.IsValid());
    EXPECT_EQ(ctx.Get(), nullptr);
}

TEST(SslCtxTest, ExplicitConstruct_Valid)
{
    const mcr::SslCtx ctx = MakeCtx();
    EXPECT_TRUE(ctx.IsValid());
    EXPECT_NE(ctx.Get(), nullptr);
}

TEST(SslCtxTest, MoveConstruct_TransfersOwnership)
{
    mcr::SslCtx src = MakeCtx();
    SSL_CTX* raw = src.Get();

    const mcr::SslCtx dst{std::move(src)};

    EXPECT_FALSE(src.IsValid());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_TRUE(dst.IsValid());
    EXPECT_EQ(dst.Get(), raw);
}

TEST(SslCtxTest, MoveAssign_TransfersOwnership)
{
    mcr::SslCtx src = MakeCtx();
    mcr::SslCtx dst = MakeCtx();
    SSL_CTX* raw = src.Get();

    dst = std::move(src);

    EXPECT_FALSE(src.IsValid());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
    EXPECT_TRUE(dst.IsValid());
    EXPECT_EQ(dst.Get(), raw);
}

TEST(SslCtxTest, Release_RelinquishesOwnership)
{
    mcr::SslCtx ctx = MakeCtx();
    SSL_CTX* raw = ctx.Get();

    SSL_CTX* released = ctx.Release();

    EXPECT_EQ(released, raw);
    EXPECT_FALSE(ctx.IsValid());

    ::SSL_CTX_free(released);
}

TEST(SslCtxTest, Reset_FreesAndInvalidates)
{
    mcr::SslCtx ctx = MakeCtx();
    ASSERT_TRUE(ctx.IsValid());

    ctx.Reset();

    EXPECT_FALSE(ctx.IsValid());
    EXPECT_EQ(ctx.Get(), nullptr);
}

TEST(SslCtxTest, Reset_OnInvalid_IsNoOp)
{
    mcr::SslCtx ctx;
    EXPECT_NO_FATAL_FAILURE(ctx.Reset());
    EXPECT_FALSE(ctx.IsValid());
}
