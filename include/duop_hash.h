/* include/duop_hash.h - CRC32 and SHA-256 (src/core/hash.c), XXH64 (src/core/hash_xxh.c) and whole-file hashing (src/core/hash_file.c). */
#pragma once
#include "duop_abi.h"

typedef struct ShaCtx {
    uint32_t state[8];
    uint64_t total;
    uint32_t buflen, pad;
    uint8_t  buf[64];
} ShaCtx;
_Static_assert(sizeof(ShaCtx) == 112 && offsetof(ShaCtx, buf) == 48, "ShaCtx layout");

uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len);   /* raw running value: seed ~0, invert at the end */
void sha256_init(ShaCtx *ctx);
void sha256_update(ShaCtx *ctx, const uint8_t *buf, size_t len);
void sha256_final_hex(ShaCtx *ctx, char *out_hex65);                   /* 64 lower-case hex digits + NUL */

/* XXH64 (seed 0), streaming: the content checksum of Zstandard frames (its low 32 bits) */
typedef struct Xxh64 {
    uint64_t v[4];
    uint64_t total;
    uint32_t buflen, pad;
    uint8_t buf[32];
} Xxh64;
void xxh64_init(Xxh64 *x);
void xxh64_update(Xxh64 *x, const uint8_t *buf, size_t len);
uint64_t xxh64_digest(const Xxh64 *x);

/* CRC32 and SHA-256 of a regular file (symlinks and non-regular files are rejected). 0, -errno from open, or -1. */
intptr_t hash_file(const char *path, uint32_t *out_crc32, char *out_sha_hex65);
