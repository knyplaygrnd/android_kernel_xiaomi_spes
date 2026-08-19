// SPDX-License-Identifier: MIT
/*
 * ucl_sha3 - SHA3 / Keccak implementation
 *
 * Based on the implementation by Markku-Juhani O. Saarinen.
 * Copyright (C) 2017 Maxim Integrated Products, Inc., All Rights Reserved.
 * Copyright (c) 2015 Markku-Juhani O. Saarinen
 *
 */
#include "ucl_hash.h"
#ifdef HASH_SHA3

#include <linux/string.h>
#include "ucl_retdefs.h"
#include "ucl_sha3.h"

#define N_ROUNDS 24
#define ROTL64(x, y)	\
	(((x) << (y)) | ((x) >> ((sizeof(u64) * 8) - (y))))

static const u64 kcf_rc[24] = {
	0x0000000000000001, 0x0000000000008082, 0x800000000000808a,
	0x8000000080008000, 0x000000000000808b, 0x0000000080000001,
	0x8000000080008081, 0x8000000000008009, 0x000000000000008a,
	0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
	0x000000008000808b, 0x800000000000008b, 0x8000000000008089,
	0x8000000000008003, 0x8000000000008002, 0x8000000000000080,
	0x000000000000800a, 0x800000008000000a, 0x8000000080008081,
	0x8000000000008080, 0x0000000080000001, 0x8000000080008008
};

static const u8 kcf_rho[24] = {
	1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
	27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44
};

static const u8 kcf_pilane[24] = {
	10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
	15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1
};

static void kcf(u64 state[25])
{
	int i, j, round;
	u64 t, c[5];

	for (round = 0; round < N_ROUNDS; round++) {
		/* Theta */
		for (i = 0; i < 5; i++)
			c[i] = state[i] ^ state[i + 5] ^ state[i + 10]
				^ state[i + 15] ^ state[i + 20];

		for (i = 0; i < 5; i++) {
			t = c[(i + 4) % 5] ^ (u64)ROTL64(c[(i + 1) % 5], 1);
			for (j = 0; j < 25; j += 5)
				state[j + i] ^= t;
		}

		/* Rho Pi */
		t = state[1];
		for (i = 0; i < 24; i++) {
			j = (int)kcf_pilane[i];
			c[0] = state[j];
			state[j] = (u64)ROTL64(t, kcf_rho[i]);
			t = c[0];
		}

		/* Chi */
		for (j = 0; j < 25; j += 5) {
			for (i = 0; i < 5; i++)
				c[i] = state[j + i];
			for (i = 0; i < 5; i++)
				state[j + i] ^= (~c[(i + 1) % 5]) & c[(i + 2) % 5];
		}

		/* Iota */
		state[0] ^= kcf_rc[round];
	}
}

int ucl_shake128_init(struct ucl_sha3_ctx *ctx)
{
	if (!ctx)
		return UCL_INVALID_INPUT;
	memset(ctx, 0, sizeof(*ctx));
	ctx->capacity_words = 2 * 128 / (8 * sizeof(u64));
	return UCL_OK;
}

int ucl_sha3_224_init(struct ucl_sha3_ctx *ctx)
{
	if (!ctx)
		return UCL_INVALID_INPUT;
	memset(ctx, 0, sizeof(*ctx));
	ctx->capacity_words = 448 / (8 * sizeof(u64));
	return UCL_OK;
}

int ucl_sha3_256_init(struct ucl_sha3_ctx *ctx)
{
	if (!ctx)
		return UCL_INVALID_INPUT;
	memset(ctx, 0, sizeof(*ctx));
	ctx->capacity_words = 512 / (8 * sizeof(u64));
	return UCL_OK;
}

int ucl_shake256_init(struct ucl_sha3_ctx *ctx)
{
	return ucl_sha3_256_init(ctx);
}

int ucl_sha3_384_init(struct ucl_sha3_ctx *ctx)
{
	if (!ctx)
		return UCL_INVALID_INPUT;
	memset(ctx, 0, sizeof(*ctx));
	ctx->capacity_words = 768 / (8 * sizeof(u64));
	return UCL_OK;
}

int ucl_sha3_512_init(struct ucl_sha3_ctx *ctx)
{
	if (!ctx)
		return UCL_INVALID_INPUT;
	memset(ctx, 0, sizeof(*ctx));
	ctx->capacity_words = 1024 / (8 * sizeof(u64));
	return UCL_OK;
}

int ucl_sha3_core(struct ucl_sha3_ctx *ctx, const u8 *buf_in, u32 len)
{
	u32 old_tail;
	size_t words;
	int tail;
	size_t i;
	const u8 *buf = buf_in;

	if (!ctx)
		return UCL_INVALID_INPUT;
	if (!buf_in)
		return UCL_INVALID_INPUT;

	old_tail = (8 - ctx->byte_index) & 7;

	if (len < old_tail) {
		while (len--)
			ctx->saved |= (u64)(*(buf++)) << ((ctx->byte_index++) * 8);
		return UCL_OK;
	}

	if (old_tail) {
		len -= old_tail;
		while (old_tail--)
			ctx->saved |= (u64)(*(buf++)) << ((ctx->byte_index++) * 8);
		ctx->s[ctx->word_index] ^= ctx->saved;
		ctx->byte_index = 0;
		ctx->saved = 0;
		if (++ctx->word_index == ((int)SHA3_SPONGE_WORDS - ctx->capacity_words)) {
			kcf(ctx->s);
			ctx->word_index = 0;
		}
	}

	words = len / sizeof(u64);
	tail = (int)(len - words * sizeof(u64));

	for (i = 0; i < words; i++, buf += sizeof(u64)) {
		const u64 t = (u64)(buf[0]) |
			      ((u64)(buf[1]) << 8) |
			      ((u64)(buf[2]) << 16) |
			      ((u64)(buf[3]) << 24) |
			      ((u64)(buf[4]) << 32) |
			      ((u64)(buf[5]) << 40) |
			      ((u64)(buf[6]) << 48) |
			      ((u64)(buf[7]) << 56);

		ctx->s[ctx->word_index] ^= t;
		if (++ctx->word_index == ((int)SHA3_SPONGE_WORDS - ctx->capacity_words)) {
			kcf(ctx->s);
			ctx->word_index = 0;
		}
	}

	while (tail--)
		ctx->saved |= (u64)(*(buf++)) << ((ctx->byte_index++) * 8);

	return UCL_OK;
}

int ucl_sha3_finish(u8 *digest, struct ucl_sha3_ctx *ctx)
{
	int i;

	if (!ctx)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;

	ctx->s[ctx->word_index] ^=
		(ctx->saved ^ ((u64)(0x02 | (1 << 2)) << (ctx->byte_index * 8)));
	ctx->s[(int)SHA3_SPONGE_WORDS - ctx->capacity_words - 1] ^=
		0x8000000000000000ULL;
	kcf(ctx->s);

	for (i = 0; i < (int)SHA3_SPONGE_WORDS; i++) {
		const u32 t1 = (u32)(ctx->s[i]);
		const u32 t2 = (u32)(ctx->s[i] >> 32);

		ctx->sb[i * 8 + 0] = (u8)(t1);
		ctx->sb[i * 8 + 1] = (u8)(t1 >> 8);
		ctx->sb[i * 8 + 2] = (u8)(t1 >> 16);
		ctx->sb[i * 8 + 3] = (u8)(t1 >> 24);
		ctx->sb[i * 8 + 4] = (u8)(t2);
		ctx->sb[i * 8 + 5] = (u8)(t2 >> 8);
		ctx->sb[i * 8 + 6] = (u8)(t2 >> 16);
		ctx->sb[i * 8 + 7] = (u8)(t2 >> 24);
	}

	for (i = 0; i < (int)SHA3_SPONGE_WORDS * 8; i++)
		digest[i] = ctx->sb[i];

	return UCL_OK;
}

int ucl_shake_finish(u8 *digest, struct ucl_sha3_ctx *ctx)
{
	int i;

	if (!ctx)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;

	ctx->s[ctx->word_index] ^=
		(ctx->saved ^ ((u64)(0x1F) << (ctx->byte_index * 8)));
	ctx->s[(int)SHA3_SPONGE_WORDS - ctx->capacity_words - 1] ^=
		0x8000000000000000ULL;
	kcf(ctx->s);

	for (i = 0; i < (int)SHA3_SPONGE_WORDS; i++) {
		const u32 t1 = (u32)(ctx->s[i]);
		const u32 t2 = (u32)(ctx->s[i] >> 32);

		ctx->sb[i * 8 + 0] = (u8)(t1);
		ctx->sb[i * 8 + 1] = (u8)(t1 >> 8);
		ctx->sb[i * 8 + 2] = (u8)(t1 >> 16);
		ctx->sb[i * 8 + 3] = (u8)(t1 >> 24);
		ctx->sb[i * 8 + 4] = (u8)(t2);
		ctx->sb[i * 8 + 5] = (u8)(t2 >> 8);
		ctx->sb[i * 8 + 6] = (u8)(t2 >> 16);
		ctx->sb[i * 8 + 7] = (u8)(t2 >> 24);
	}

	for (i = 0; i < (int)SHA3_SPONGE_WORDS * 8; i++)
		digest[i] = ctx->sb[i];

	return UCL_OK;
}

int ucl_sha3_224(u8 *digest, u8 *msg, u32 msg_len)
{
	struct ucl_sha3_ctx ctx;

	if (!msg)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;
	if (ucl_sha3_224_init(&ctx) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_core(&ctx, msg, msg_len) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_finish(digest, &ctx) != UCL_OK)
		return UCL_ERROR;

	return UCL_OK;
}

int ucl_sha3_256(u8 *digest, u8 *msg, u32 msg_len)
{
	struct ucl_sha3_ctx ctx;

	if (!msg)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;
	if (ucl_sha3_256_init(&ctx) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_core(&ctx, msg, msg_len) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_finish(digest, &ctx) != UCL_OK)
		return UCL_ERROR;

	return UCL_OK;
}

int ucl_shake128(u8 *digest, u8 *msg, u32 msg_len)
{
	struct ucl_sha3_ctx ctx;

	if (!msg)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;
	if (ucl_shake128_init(&ctx) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_core(&ctx, msg, msg_len) != UCL_OK)
		return UCL_ERROR;
	if (ucl_shake_finish(digest, &ctx) != UCL_OK)
		return UCL_ERROR;

	return UCL_OK;
}

int ucl_sha3_384(u8 *digest, u8 *msg, u32 msg_len)
{
	struct ucl_sha3_ctx ctx;

	if (!msg)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;
	if (ucl_sha3_384_init(&ctx) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_core(&ctx, msg, msg_len) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_finish(digest, &ctx) != UCL_OK)
		return UCL_ERROR;

	return UCL_OK;
}

int ucl_sha3_512(u8 *digest, u8 *msg, u32 msg_len)
{
	struct ucl_sha3_ctx ctx;

	if (!msg)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;
	if (ucl_sha3_512_init(&ctx) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_core(&ctx, msg, msg_len) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_finish(digest, &ctx) != UCL_OK)
		return UCL_ERROR;

	return UCL_OK;
}

int ucl_shake256(u8 *digest, u8 *msg, u32 msg_len)
{
	struct ucl_sha3_ctx ctx;

	if (!msg)
		return UCL_INVALID_INPUT;
	if (!digest)
		return UCL_INVALID_OUTPUT;
	if (ucl_shake256_init(&ctx) != UCL_OK)
		return UCL_ERROR;
	if (ucl_sha3_core(&ctx, msg, msg_len) != UCL_OK)
		return UCL_ERROR;
	if (ucl_shake_finish(digest, &ctx) != UCL_OK)
		return UCL_ERROR;

	return UCL_OK;
}

#endif /* HASH_SHA3 */
