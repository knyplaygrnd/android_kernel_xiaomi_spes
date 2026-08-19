/* SPDX-License-Identifier: MIT */
/*
 * ucl_sha3 - SHA3 / Keccak implementation
 *
 * Based on the implementation by Markku-Juhani O. Saarinen.
 * Copyright (C) 2017 Maxim Integrated Products, Inc., All Rights Reserved.
 * Copyright (c) 2015 Markku-Juhani O. Saarinen
 *
 */
#ifndef __UCL_SHA3_H__
#define __UCL_SHA3_H__

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCL_SHA3_224_HASHSIZE	28
#define UCL_SHA3_256_HASHSIZE	32
#define UCL_SHA3_384_HASHSIZE	48
#define UCL_SHA3_512_HASHSIZE	64
#define UCL_SHA3_MAX_PERMSIZE	25
#define UCL_SHA3_MAXQRATE_QWORDS	24

#define UCL_SHA3	2
#define UCL_SHA3_224	9
#define UCL_SHA3_256	10
#define UCL_SHA3_384	11
#define UCL_SHA3_512	12

/**
 * SHA3 Algorithm context.
 */

/** @file ucl_sha3
 * @defgroup UCL_SHA3 SHA3
 * Secure Hash Algorithm 3, from FIPS 202.
 *
 * @par Header:
 * @link ucl_sha3 ucl_sha3 @endlink
 *
 * SHA-3 is a data-digest algorithm and a component of
 * the Standard FIPS 202.
 * The algorithm takes as input a data of arbitrary length and
 * produces as output 224-bit, 256-bit, 384-bit or 512-bit "fingerprint" or "data digest"
 * of the input.@n
 * @n
 *
 * <b>SHA3 Descriptor:</b> @n
 * @li Hash length : 224/256/384/512 bits
 *
 * @ingroup UCL_HASH
 */

/** <b>The SHA3 context</b>.
 * This structure is associated to the 'step by step' process.
 *
 * @ingroup UCL_SHA3
 */

struct sha3_ctx {
	/* 1600 bits algorithm hashing state */
	u64 hash[UCL_SHA3_MAX_PERMSIZE];
	/* 1536-bit buffer for leftovers */
	u64 message[UCL_SHA3_MAXQRATE_QWORDS];
	/* count of bytes in the message[] buffer */
	u32 rest;
	/* size of a message block processed at once */
	u32 block_size;
};

#define SHA3_STATE_BITS		1600
#define SHA3_STATE_BYTES	(SHA3_STATE_BITS / 8)
#define SHA3_SPONGE_WORDS	(SHA3_STATE_BYTES / sizeof(u64))

struct ucl_sha3_ctx {
	u64 saved;
	union {
		u64 s[SHA3_SPONGE_WORDS];
		u8 sb[SHA3_SPONGE_WORDS * 8];
	};
/* 0..7--the next byte after the set one (starts from 0; 0--none are buffered) */
	int byte_index;
/* 0..24--the next word to integrate input (starts from 0) */
	int word_index;
/* the double size of the hash output in words (e.g. 16 for Keccak 512) */
	int capacity_words;
};

/* methods for calculating the hash function */

/*============================================================================*/
/** <b>SHA3-224 Init</b>.
 * The initialisation of SHA3-224.
 *
 * @param[in,out] context Pointer to the context
 *
 * @return Error code
 *
 * @retval #UCL_OK            if no error occurred
 * @retval #UCL_INVALID_INPUT if @p context is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_224_init(struct ucl_sha3_ctx *ctx);

/*============================================================================*/
/** <b>SHA3-256 Init</b>.
 * The initialisation of SHA3-256.
 *
 * @param[in,out] context Pointer to the context
 *
 * @return Error code
 *
 * @retval #UCL_OK            if no error occurred
 * @retval #UCL_INVALID_INPUT if @p context is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_256_init(struct ucl_sha3_ctx *ctx);

/*============================================================================*/
/** <b>SHA3-384 Init</b>.
 * The initialisation of SHA3-384.
 *
 * @param[in,out] context Pointer to the context
 *
 * @return Error code
 *
 * @retval #UCL_OK            if no error occurred
 * @retval #UCL_INVALID_INPUT if @p context is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_384_init(struct ucl_sha3_ctx *ctx);

/*============================================================================*/
/** <b>SHA3-512 Init</b>.
 * The initialisation of SHA3-512.
 *
 * @param[in,out] context Pointer to the context
 *
 * @return Error code
 *
 * @retval #UCL_OK            if no error occurred
 * @retval #UCL_INVALID_INPUT if @p context is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_512_init(struct ucl_sha3_ctx *ctx);

/*============================================================================*/
/** <b>SHA3 Core</b>.
 * The core of SHA3, common to all SHA3 hash functions.
 *
 * @param[in,out] context      Pointer to the context
 * @param[in]     data         Pointer to the data
 * @param[in]     data_byteLen Data byte length
 *
 * @warning #ucl_sha3-*_init must be processed before, and
 *          #ucl_sha3_finish should be processed to get the final hash.
 *
 * @return Error code
 *
 * @retval #UCL_OK  if no error occurred
 * @retval #UCL_NOP if @p data_byteLen = 0 or if @p data is the pointer #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_core(struct ucl_sha3_ctx *ctx, const u8 *buf_in, u32 len);

/*============================================================================*/
/** <b>SHA3 Finish</b>.
 * Finish the process of SHA3, common to all SHA3 hash functions.
 *
 * @pre Hash byte length is equal to 28/32/48 or 64 bytes
 *
 * @param[out]    hash Pointer to the digest
 * @param[in,out] context Pointer to the context
 *
 * @warning #ucl_sha3_*_init and #ucl_sha3_core must be processed before.
 *
 * @return Error code
 *
 * @retval #UCL_OK             if no error occurred
 * @retval #UCL_INVALID_OUTPUT if @p hash is the pointer #NULL
 * @retval #UCL_INVALID_INPUT  if @p context is the pointer #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_finish(u8 *digest, struct ucl_sha3_ctx *ctx);

/*============================================================================*/
/** <b>SHA3-224</b>.
 * The complete process of SHA3-224.
 *
 * @pre Hash byte length is equal to 28
 *
 * @param[out] hash         Pointer to the digest
 * @param[in]  data         Pointer to the data
 * @param[in]  data_byteLen Data byte length
 *
 * @return Error code
 *
 * @retval #UCL_OK             if no error occurred
 * @retval #UCL_INVALID_OUTPUT if @p hash is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_224(u8 *digest, u8 *msg, u32 msg_len);

/*============================================================================*/
/** <b>SHA3-256</b>.
 * The complete process of SHA3-256.
 *
 * @pre Hash byte length is equal to 32
 *
 * @param[out] hash         Pointer to the digest
 * @param[in]  data         Pointer to the data
 * @param[in]  data_byteLen Data byte length
 *
 * @return Error code
 *
 * @retval #UCL_OK             if no error occurred
 * @retval #UCL_INVALID_OUTPUT if @p hash is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_256(u8 *digest, u8 *msg, u32 msg_len);

/*============================================================================*/
/** <b>SHA3-384</b>.
 * The complete process of SHA3-384.
 *
 * @pre Hash byte length is equal to 48
 *
 * @param[out] hash         Pointer to the digest
 * @param[in]  data         Pointer to the data
 * @param[in]  data_byteLen Data byte length
 *
 * @return Error code
 *
 * @retval #UCL_OK             if no error occurred
 * @retval #UCL_INVALID_OUTPUT if @p hash is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_384(u8 *digest, u8 *msg, u32 msg_len);

/*============================================================================*/
/** <b>SHA3-512</b>.
 * The complete process of SHA3-512.
 *
 * @pre Hash byte length is equal to 64
 *
 * @param[out] hash         Pointer to the digest
 * @param[in]  data         Pointer to the data
 * @param[in]  data_byteLen Data byte length
 *
 * @return Error code
 *
 * @retval #UCL_OK             if no error occurred
 * @retval #UCL_INVALID_OUTPUT if @p hash is #NULL
 *
 * @ingroup UCL_SHA3
 */
int ucl_sha3_512(u8 *digest, u8 *msg, u32 msg_len);
int ucl_shake128_init(struct ucl_sha3_ctx *ctx);
int ucl_shake256_init(struct ucl_sha3_ctx *ctx);
int ucl_shake_finish(u8 *hash, struct ucl_sha3_ctx *ctx);
int ucl_shake128(u8 *digest, u8 *msg, u32 msg_len);
int ucl_shake256(u8 *digest, u8 *msg, u32 msg_len);
#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* __UCL_SHA3_H__ */
