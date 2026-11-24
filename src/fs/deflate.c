/* src/fs/deflate.c - LZ77 matching for the DEFLATE compressor (freestanding C23); the Huffman side is deflate_huff.c.
 *
 * A 64 KiB window slides by 32 KiB; strings of three bytes are hashed into chains of earlier positions, and the longest match within
 * 32768 - 262 bytes is taken (greedy at levels 1-3, one step lazy at 4-9, the effort per level as zlib's table). Positions are only
 * examined while at least 262 bytes of lookahead are in the window - until the stream ends - so the choices, and with them the output,
 * do not depend on how the caller cut the input into writes. */
#include "duop_deflate_int.h"

static const struct { uint16_t good, lazy, nice, chain; bool lazy_mode; } config[10] = {
    {0, 0, 0, 0, false}, {4, 4, 8, 4, false}, {4, 5, 16, 8, false}, {4, 6, 32, 32, false}, {4, 4, 16, 16, true},
    {8, 16, 32, 32, true}, {8, 16, 128, 128, true}, {8, 32, 128, 256, true}, {32, 128, 258, 1024, true}, {32, 258, 258, 4096, true}};

Deflate *deflate_new(int level, const CodecSink *sink)
{
    if (level < 0) level = 6;
    if (level > 9) level = 9;
    Deflate *d = mem_alloc(sizeof *d);
    if (!d) return nullptr;
    d->sink = sink;
    d->level = level;
    d->win = mem_alloc(2 * DEF_WSIZE);
    d->out = mem_alloc(DEF_OUT + 64);
    if (level) {
        d->head = mem_alloc(DEF_HASH_SIZE * sizeof(uint16_t));
        d->prev = mem_alloc(DEF_WSIZE * sizeof(uint16_t));
        d->lc = mem_alloc(DEF_SYMS);
        d->dist = mem_alloc(DEF_SYMS * sizeof(uint16_t));
    }
    if (!d->win || !d->out || (level && (!d->head || !d->prev || !d->lc || !d->dist))) {
        deflate_free(d);
        return nullptr;
    }
    d->good = config[level].good;
    d->lazy = config[level].lazy;
    d->nice = config[level].nice;
    d->chain = config[level].chain;
    d->lazy_mode = config[level].lazy_mode;
    d->match_length = d->prev_length = DEF_MIN_MATCH - 1;
    deflate_tables_init(d);
    return d;
}

void deflate_free(Deflate *d)
{
    if (!d) return;
    mem_free(d->win);
    mem_free(d->out);
    mem_free(d->head);
    mem_free(d->prev);
    mem_free(d->lc);
    mem_free(d->dist);
    mem_free(d);
}

static inline uint32_t hash3(const uint8_t *p) { return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & (DEF_HASH_SIZE - 1); }

/* enters the string at window position s into its chain; returns the previous head of the chain */
static inline uint32_t insert_string(Deflate *d, uint32_t s)
{
    uint32_t h = hash3(d->win + s), old = d->head[h];
    d->prev[s & DEF_WMASK] = (uint16_t)old;
    d->head[h] = (uint16_t)s;
    return old;
}

/* the longest match of the string at strstart against the chain from cur_match, longer than prev_len; sets match_start; returns its length */
static uint32_t longest_match(Deflate *d, uint32_t cur_match, uint32_t prev_len)
{
    uint32_t chain = d->chain, best = prev_len, nice = d->nice;
    const uint8_t *scan = d->win + d->strstart;
    uint32_t limit = d->strstart > DEF_MAX_DIST ? d->strstart - DEF_MAX_DIST : 0;
    if (prev_len >= d->good) chain >>= 2;
    if (nice > d->lookahead) nice = d->lookahead;
    do {
        const uint8_t *match = d->win + cur_match;
        if (match[best] != scan[best] || match[best - 1] != scan[best - 1] || match[0] != scan[0] || match[1] != scan[1]) continue;
        uint32_t len = 2;
        while (len < DEF_MAX_MATCH && scan[len] == match[len]) len++;
        if (len > best) {
            d->match_start = cur_match;
            best = len;
            if (len >= nice) break;
        }
    } while ((cur_match = d->prev[cur_match & DEF_WMASK]) > limit && --chain != 0);
    return best < d->lookahead ? best : d->lookahead;
}

static int flush_block(Deflate *d, bool last)
{
    const uint8_t *stored = d->block_start >= 0 ? d->win + d->block_start : nullptr;
    size_t len = (size_t)((int64_t)d->strstart - d->block_start);
    int r = deflate_emit_block(d, stored, len, last);
    d->block_start = d->strstart;
    return r;
}

static inline void tally_lit(Deflate *d, uint8_t c)
{
    d->dist[d->nsym] = 0;
    d->lc[d->nsym++] = c;
}

static inline void tally_match(Deflate *d, uint32_t dist, uint32_t len)
{
    d->dist[d->nsym] = (uint16_t)dist;
    d->lc[d->nsym++] = (uint8_t)(len - DEF_MIN_MATCH);
}

/* greedy: take the longest match at every position, else a literal */
static int run_fast(Deflate *d, bool flush)
{
    while (d->lookahead >= DEF_MIN_LOOKAHEAD || (flush && d->lookahead)) {
        uint32_t head = d->lookahead >= DEF_MIN_MATCH ? insert_string(d, d->strstart) : 0;
        d->match_length = 0;
        if (head && d->strstart - head <= DEF_MAX_DIST) d->match_length = longest_match(d, head, DEF_MIN_MATCH - 1);
        if (d->match_length >= DEF_MIN_MATCH) {
            tally_match(d, d->strstart - d->match_start, d->match_length);
            d->lookahead -= d->match_length;
            for (uint32_t k = 1; k < d->match_length; k++)
                if (d->lookahead + d->match_length - k >= DEF_MIN_MATCH) (void)insert_string(d, d->strstart + k);
            d->strstart += d->match_length;
        } else {
            tally_lit(d, d->win[d->strstart]);
            d->lookahead--;
            d->strstart++;
        }
        if (d->nsym == DEF_SYMS) {
            int r = flush_block(d, false);
            if (r) return r;
        }
    }
    return 0;
}

/* lazy: a match is taken only when the next position has none that is longer */
static int run_lazy(Deflate *d, bool flush)
{
    while (d->lookahead >= DEF_MIN_LOOKAHEAD || (flush && d->lookahead)) {
        uint32_t head = d->lookahead >= DEF_MIN_MATCH ? insert_string(d, d->strstart) : 0;
        d->prev_length = d->match_length;
        d->prev_match = d->match_start;
        d->match_length = DEF_MIN_MATCH - 1;
        if (head && d->prev_length < d->lazy && d->strstart - head <= DEF_MAX_DIST) {
            d->match_length = longest_match(d, head, d->prev_length < DEF_MIN_MATCH - 1 ? DEF_MIN_MATCH - 1 : d->prev_length);
            if (d->match_length <= 5 && d->match_length == DEF_MIN_MATCH && d->strstart - d->match_start > 4096) d->match_length = DEF_MIN_MATCH - 1;
        }
        bool full = false;
        if (d->prev_length >= DEF_MIN_MATCH && d->match_length <= d->prev_length) {
            uint32_t max_insert = d->strstart + d->lookahead - DEF_MIN_MATCH;
            tally_match(d, d->strstart - 1 - d->prev_match, d->prev_length);
            d->lookahead -= d->prev_length - 1;
            d->prev_length -= 2;
            do {
                if (++d->strstart <= max_insert) (void)insert_string(d, d->strstart);
            } while (--d->prev_length);
            d->match_available = false;
            d->match_length = DEF_MIN_MATCH - 1;
            d->strstart++;
            full = d->nsym == DEF_SYMS;
        } else if (d->match_available) {
            tally_lit(d, d->win[d->strstart - 1]);
            full = d->nsym == DEF_SYMS;
            if (full) {
                int r = flush_block(d, false);
                if (r) return r;
                full = false;
            }
            d->strstart++;
            d->lookahead--;
        } else {
            d->match_available = true;
            d->strstart++;
            d->lookahead--;
        }
        if (full) {
            int r = flush_block(d, false);
            if (r) return r;
        }
    }
    return 0;
}

/* moves the upper half of the window down, and the positions in the hash chains with it */
static void slide(Deflate *d)
{
    __builtin_memcpy(d->win, d->win + DEF_WSIZE, DEF_WSIZE);
    d->strstart -= DEF_WSIZE;
    d->block_start -= DEF_WSIZE;
    d->match_start = d->match_start >= DEF_WSIZE ? d->match_start - DEF_WSIZE : 0;
    d->prev_match = d->prev_match >= DEF_WSIZE ? d->prev_match - DEF_WSIZE : 0;
    for (uint32_t i = 0; i < DEF_HASH_SIZE; i++) d->head[i] = d->head[i] >= DEF_WSIZE ? (uint16_t)(d->head[i] - DEF_WSIZE) : 0;
    for (uint32_t i = 0; i < DEF_WSIZE; i++) d->prev[i] = d->prev[i] >= DEF_WSIZE ? (uint16_t)(d->prev[i] - DEF_WSIZE) : 0;
}

static int run(Deflate *d, bool flush) { return d->lazy_mode ? run_lazy(d, flush) : run_fast(d, flush); }

/* level 0: stored blocks of 65535 bytes, filled in the window buffer */
static int stored_write(Deflate *d, const uint8_t *buf, size_t len)
{
    while (len) {
        size_t take = 65535 - d->lookahead < len ? 65535 - d->lookahead : len;
        __builtin_memcpy(d->win + d->lookahead, buf, take);
        d->lookahead += (uint32_t)take;
        buf += take;
        len -= take;
        if (d->lookahead == 65535) {
            int r = deflate_emit_stored(d, d->win, 65535, false);
            d->lookahead = 0;
            if (r) return r;
        }
    }
    return 0;
}

int deflate_write(Deflate *d, const uint8_t *buf, size_t len)
{
    if (d->err) return d->err;
    if (!d->level) return d->err = stored_write(d, buf, len);
    while (len) {
        if (d->strstart >= DEF_WSIZE + DEF_MAX_DIST) slide(d);
        uint32_t room = 2 * DEF_WSIZE - d->strstart - d->lookahead;
        size_t take = room < len ? room : len;
        __builtin_memcpy(d->win + d->strstart + d->lookahead, buf, take);
        d->lookahead += (uint32_t)take;
        buf += take;
        len -= take;
        int r = run(d, false);
        if (r) return d->err = r;
    }
    return 0;
}

void deflate_reset(Deflate *d)
{
    d->err = 0;
    d->strstart = d->lookahead = d->nsym = 0;
    d->block_start = 0;
    d->match_start = d->prev_match = 0;
    d->match_length = d->prev_length = DEF_MIN_MATCH - 1;
    d->match_available = false;
    d->outn = 0;
    d->bitbuf = 0;
    d->bitcnt = 0;
    if (d->head) __builtin_memset(d->head, 0, DEF_HASH_SIZE * sizeof(uint16_t));
}

int deflate_finish(Deflate *d)
{
    if (d->err) return d->err;
    int r;
    if (!d->level) {
        r = deflate_emit_stored(d, d->win, d->lookahead, true);
        d->lookahead = 0;
    } else {
        r = run(d, true);
        if (!r && d->match_available) {                           /* the position behind the last lazy step still holds its literal */
            tally_lit(d, d->win[d->strstart - 1]);
            d->match_available = false;
        }
        if (!r) r = flush_block(d, true);
    }
    if (!r) r = deflate_flush_bits(d);
    return d->err = r;
}
