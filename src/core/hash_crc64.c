/* src/core/hash_crc64.c - CRC-64/XZ (ECMA-182 polynomial, reflected; freestanding C23): the integrity check type 4 of the .xz format.
 * The 256-entry table is built on first use (a pure function of the polynomial, so a repeated build is harmless). */
#include "duop_hash.h"

#define POLY 0xC96C5795D7870F42ull

static uint64_t g_table[256];
static bool g_ready;

static void build(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint64_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ POLY : c >> 1;
        g_table[i] = c;
    }
    g_ready = true;
}

uint64_t crc64_update(uint64_t crc, const uint8_t *buf, size_t len)
{
    if (!g_ready) build();
    for (size_t i = 0; i < len; i++) crc = g_table[(crc ^ buf[i]) & 0xFF] ^ crc >> 8;
    return crc;
}
