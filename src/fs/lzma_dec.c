/* src/fs/lzma_dec.c - the LZMA decoder proper: range decoder, literals, matches, repeat matches (freestanding C23).
 *
 * One symbol at a time into the ring of lzma2_dec.c. The probabilities are 11-bit counters adapted by 5 bits; a match is a length and a
 * distance, the latter either one of the last four (repeat) or a slot with extra bits. Everything is bounded: the range decoder reads
 * only its chunk, a distance must lie inside the history that exists, an end marker is an error (LZMA2 has none). */
#include "duop_lzma_int.h"

enum { PROB_INIT = 1024, MATCH_MIN = 2, END_MARK = 0xFFFFFFFFu };

static inline void norm(Lzma *z)
{
    if (z->range < (1u << 24)) {
        uint32_t b = 0;
        if (z->in < z->end) b = *z->in++;
        else z->bad = true;
        z->range <<= 8;
        z->code = z->code << 8 | b;
    }
}

static inline uint32_t bit(Lzma *z, uint16_t *p)
{
    norm(z);
    uint32_t bound = (z->range >> 11) * *p;
    if (z->code < bound) {
        z->range = bound;
        *p += (2048 - *p) >> 5;
        return 0;
    }
    z->range -= bound;
    z->code -= bound;
    *p -= *p >> 5;
    return 1;
}

static inline uint32_t tree(Lzma *z, uint16_t *probs, uint32_t nbits)
{
    uint32_t m = 1;
    for (uint32_t i = 0; i < nbits; i++) m = (m << 1) + bit(z, probs + m);
    return m - (1u << nbits);
}

static inline uint32_t tree_rev(Lzma *z, uint16_t *probs, uint32_t nbits, uint32_t v)
{
    uint32_t m = 1;
    for (uint32_t i = 0; i < nbits; i++) {
        uint32_t b = bit(z, probs + m);
        m = (m << 1) + b;
        v += b << i;
    }
    return v;
}

static uint32_t direct(Lzma *z, uint32_t nbits, uint32_t v)
{
    do {
        norm(z);
        z->range >>= 1;
        z->code -= z->range;
        uint32_t mask = 0u - (z->code >> 31);
        z->code += z->range & mask;
        v = (v << 1) + (mask + 1);
    } while (--nbits);
    return v;
}

static uint32_t decode_len(Lzma *z, LzmaLen *l, uint32_t pos_state)
{
    if (!bit(z, &l->choice)) return MATCH_MIN + tree(z, l->low[pos_state], 3);
    if (!bit(z, &l->choice2)) return MATCH_MIN + 8 + tree(z, l->mid[pos_state], 3);
    return MATCH_MIN + 16 + tree(z, l->high, 8);
}

/* the distance (stored minus one) of a new match of length len */
static uint32_t decode_dist(Lzma *z, uint32_t len)
{
    uint32_t slot = tree(z, z->dist_slot[len - MATCH_MIN < 4 ? len - MATCH_MIN : 3], 6);
    if (slot < 4) return slot;
    uint32_t limit = (slot >> 1) - 1, d = 2 | (slot & 1);
    if (slot < 14) {
        d <<= limit;
        return tree_rev(z, z->dist_special + d - slot, limit, d);
    }
    d = direct(z, limit - 4, d) << 4;
    return tree_rev(z, z->dist_align, 4, d);
}

static void init_probs(uint16_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = PROB_INIT;
}

bool lzma_set_props(Lzma *z, uint32_t props)
{
    if (props > (4 * 5 + 4) * 9 + 8) return false;
    z->lc = props % 9;
    props /= 9;
    z->lp = props % 5;
    z->pb = props / 5;
    return z->lc + z->lp <= 4;
}

void lzma_reset_state(Lzma *z)
{
    init_probs(&z->is_match[0][0], 12 * 16);
    init_probs(z->is_rep, 12);
    init_probs(z->is_rep0, 12);
    init_probs(z->is_rep1, 12);
    init_probs(z->is_rep2, 12);
    init_probs(&z->is_rep0_long[0][0], 12 * 16);
    init_probs(&z->dist_slot[0][0], 4 * 64);
    init_probs(z->dist_special, 114);
    init_probs(z->dist_align, 16);
    init_probs((uint16_t *)&z->mlen, sizeof z->mlen / 2);
    init_probs((uint16_t *)&z->rlen, sizeof z->rlen / 2);
    init_probs(z->lit, (size_t)0x300 << (z->lc + z->lp));
    z->state = 0;
    z->rep0 = z->rep1 = z->rep2 = z->rep3 = 0;
}

int lzma_rc_init(Lzma *z, const uint8_t *in, size_t n)
{
    if (n < 5 || in[0] != 0) return -EINVAL;
    z->range = 0xFFFFFFFFu;
    z->code = (uint32_t)in[1] << 24 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 8 | in[4];
    z->in = in + 5;
    z->end = in + n;
    z->bad = false;
    return 0;
}

bool lzma_rc_finished(const Lzma *z) { return z->in == z->end && z->code == 0 && !z->bad; }

static inline uint8_t back(const Lzma *z, uint32_t d)
{
    return z->buf[z->pos > d ? z->pos - d - 1 : z->pos + z->size - d - 1];
}

static void literal(Lzma *z)
{
    uint32_t prev = z->full ? back(z, 0) : 0;
    uint16_t *probs = z->lit + 0x300u * (((z->pos & ((1u << z->lp) - 1)) << z->lc) + (prev >> (8 - z->lc)));
    uint32_t sym = 1;
    if (z->state >= 7) {                                          /* after a match: the byte at rep0 steers the first bits */
        uint32_t mb = back(z, z->rep0), off = 0x100;
        do {
            mb <<= 1;
            uint32_t mbit = mb & off;
            if (bit(z, probs + off + mbit + sym)) {
                sym = (sym << 1) + 1;
                off &= mbit;
            } else {
                sym <<= 1;
                off &= ~mbit;
            }
        } while (sym < 0x100);
    } else {
        do sym = (sym << 1) + bit(z, probs + sym); while (sym < 0x100);
    }
    z->buf[z->pos++] = (uint8_t)sym;
    if (z->full < z->pos) z->full = z->pos;
    z->state = z->state < 4 ? 0 : z->state < 10 ? z->state - 3 : z->state - 6;
}

/* a repeat match: which of the four distances, then the length (1 for a "short rep") */
static uint32_t rep_match(Lzma *z, uint32_t pos_state)
{
    uint32_t st = z->state;
    if (!bit(z, &z->is_rep0[st])) {
        if (!bit(z, &z->is_rep0_long[st][pos_state])) {
            z->state = st < 7 ? 9 : 11;
            return 1;
        }
    } else {
        uint32_t d;
        if (!bit(z, &z->is_rep1[st])) {
            d = z->rep1;
        } else {
            if (!bit(z, &z->is_rep2[st])) {
                d = z->rep2;
            } else {
                d = z->rep3;
                z->rep3 = z->rep2;
            }
            z->rep2 = z->rep1;
        }
        z->rep1 = z->rep0;
        z->rep0 = d;
    }
    z->state = st < 7 ? 8 : 11;
    return decode_len(z, &z->rlen, pos_state);
}

/* copies up to `limit - pos` bytes of the match at rep0; the rest stays pending */
static void copy_match(Lzma *z, uint32_t len, size_t limit)
{
    size_t n = limit - z->pos < len ? limit - z->pos : len;
    size_t src = z->pos > z->rep0 ? z->pos - z->rep0 - 1 : z->pos + z->size - z->rep0 - 1;
    z->pending = len - (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        z->buf[z->pos++] = z->buf[src++];
        if (src == z->size) src = 0;
    }
    if (z->full < z->pos) z->full = z->pos;
}

int lzma_run(Lzma *z, size_t limit)
{
    if (z->pending) copy_match(z, z->pending, limit);
    while (z->pos < limit) {
        uint32_t pos_state = (uint32_t)z->pos & ((1u << z->pb) - 1);
        if (!bit(z, &z->is_match[z->state][pos_state])) {
            literal(z);
        } else {
            uint32_t len;
            if (bit(z, &z->is_rep[z->state])) {
                len = rep_match(z, pos_state);
            } else {
                z->rep3 = z->rep2;
                z->rep2 = z->rep1;
                z->rep1 = z->rep0;
                len = decode_len(z, &z->mlen, pos_state);
                z->state = z->state < 7 ? 7 : 10;
                z->rep0 = decode_dist(z, len);
                if (z->rep0 == END_MARK) return -EINVAL;           /* LZMA2 has no end marker */
            }
            if (z->rep0 >= z->full || z->bad) return -EINVAL;      /* a distance beyond the history */
            copy_match(z, len, limit);
        }
        if (z->bad) return -EINVAL;
    }
    norm(z);                                                       /* leave the range decoder normalised, as the chunk end test expects */
    return z->bad ? -EINVAL : 0;
}
