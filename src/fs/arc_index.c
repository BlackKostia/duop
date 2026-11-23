/* src/fs/arc_index.c - the entry table of an archive: growth, name normalisation, sorted order, lookup, folder listing (freestanding C23).
 *
 * Normalisation (what extraction would see): leading '/' and "./" go, empty and "." components go, a ".." component marks the member
 * unsafe (kept so a test or report can count it, but never listed or unpacked), a trailing '/' or the directory flag makes it a
 * directory. Names that normalise to nothing ("./", "/") are skipped. Every table grows by doubling through mem_realloc; an add that
 * fails leaves the table untouched. See include/duop_arcidx.h. */
#include "duop_arcidx.h"
#include "duop_config.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"

void arc_idx_init(ArcIndex *x) { mem_zero(x, sizeof *x); }

void arc_idx_free(ArcIndex *x)
{
    mem_free(x->items);
    mem_free(x->names);
    mem_free(x->order);
    mem_zero(x, sizeof *x);
}

const char *arc_item_name(const ArcIndex *x, const ArcItem *it) { return x->names + it->name_off; }

const char *arc_item_link(const ArcIndex *x, const ArcItem *it)
{
    return it->link_off == UINT32_MAX ? nullptr : x->names + it->link_off;
}

/* room for `need` more bytes in the arena */
static int names_reserve(ArcIndex *x, uint64_t need)
{
    if (x->names_len + need <= x->names_cap) return 0;
    uint64_t cap = x->names_cap ? x->names_cap : 4096;
    while (cap < x->names_len + need) cap *= 2;
    if (cap > 0xFFFFFFF0u) return -EFBIG;                                      /* offsets are 32 bit */
    char *p = mem_realloc(x->names, cap);
    if (!p) return -ENOMEM;
    x->names = p;
    x->names_cap = cap;
    return 0;
}

static int items_reserve(ArcIndex *x)
{
    if (x->count < x->cap) return 0;
    if (x->count >= ARC_IDX_MAX_ITEMS) return -EFBIG;
    uint32_t cap = x->cap ? x->cap * 2 : 256;
    ArcItem *p = mem_realloc(x->items, (size_t)cap * sizeof *p);
    if (!p) return -ENOMEM;
    x->items = p;
    x->cap = cap;
    return 0;
}

/* appends the normalised form of raw to the arena (NUL-terminated, at names_len); returns its length, 0 when nothing is left */
static size_t normalise_into(char *dst, const char *raw, size_t n, bool *is_dir, bool *unsafe)
{
    size_t out = 0;
    *unsafe = false;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && raw[j] != '/') j++;
        size_t len = j - i;
        if (len == 2 && raw[i] == '.' && raw[i + 1] == '.') *unsafe = true;
        if (len && !(len == 1 && raw[i] == '.')) {
            for (size_t k = 0; k < len; k++) dst[out++] = raw[i + k];
            dst[out++] = '/';
        }
        i = j + 1;
    }
    *is_dir = n && raw[n - 1] == '/';
    return out;
}

int arc_idx_add(ArcIndex *x, const char *name, size_t name_len, const char *link, size_t link_len, const ArcItem *proto)
{
    if (name_len > ARC_IDX_MAX_NAME || link_len > ARC_IDX_MAX_NAME) return -ENAMETOOLONG;
    for (size_t i = 0; i < name_len; i++)
        if (!name[i]) return -EINVAL;
    for (size_t i = 0; i < link_len; i++)
        if (!link[i]) return -EINVAL;
    int r = items_reserve(x);
    if (r) return r;
    if ((r = names_reserve(x, name_len + 2 + (link ? link_len + 1 : 0)))) return r;
    char *dst = x->names + x->names_len;
    bool is_dir, unsafe;
    size_t n = normalise_into(dst, name, name_len, &is_dir, &unsafe);
    if (!n) return 0;                                                         /* "./", "/": the root, nothing to list */
    is_dir = is_dir || (proto->flags & ARC_F_DIR);
    if (!is_dir) n--;                                                         /* drop the separator behind the last component */
    if (n >= PATH_MAX) return -ENAMETOOLONG;
    dst[n] = 0;
    uint32_t link_off = UINT32_MAX;
    uint64_t next = x->names_len + n + 1;
    if (link) {
        mem_copy(x->names + next, link, link_len);
        x->names[next + link_len] = 0;
        link_off = (uint32_t)next;
        next += link_len + 1;
    }
    ArcItem *it = &x->items[x->count++];
    *it = *proto;
    it->name_off = (uint32_t)x->names_len;
    it->link_off = link_off;
    it->flags = (uint16_t)(it->flags | (is_dir ? ARC_F_DIR : 0) | (unsafe ? ARC_F_UNSAFE : 0));
    x->unsafe += unsafe;
    x->names_len = next;
    return 0;
}

static int name_cmp(const ArcIndex *x, uint32_t a, uint32_t b)
{
    return str_cmp(x->names + x->items[a].name_off, x->names + x->items[b].name_off);
}

static bool is_sorted(const ArcIndex *x, const uint32_t *o, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
        if (name_cmp(x, o[i - 1], o[i]) > 0) return false;
    return true;
}

static void merge_pass(const ArcIndex *x, const uint32_t *src, uint32_t *dst, uint32_t n, uint32_t width)
{
    for (uint32_t lo = 0; lo < n; lo += 2 * width) {
        uint32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
        uint32_t i = lo, j = mid, k = lo;
        while (i < mid && j < hi) dst[k++] = name_cmp(x, src[j], src[i]) < 0 ? src[j++] : src[i++];   /* stable: left unless right is smaller */
        while (i < mid) dst[k++] = src[i++];
        while (j < hi) dst[k++] = src[j++];
    }
}

int arc_idx_finish(ArcIndex *x)
{
    uint32_t n = x->count;
    if (!n) {
        mem_free(x->order);
        x->order = nullptr;
        x->unique = 0;
        return 0;
    }
    uint32_t *a = mem_alloc((size_t)n * sizeof *a), *b = mem_alloc((size_t)n * sizeof *b);
    if (!a || !b) {
        mem_free(a);
        mem_free(b);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < n; i++) a[i] = i;
    if (!is_sorted(x, a, n)) {
        for (uint32_t width = 1; width < n; width <<= 1) {
            merge_pass(x, a, b, n, width);
            uint32_t *t = a;
            a = b;
            b = t;
        }
    }
    uint32_t u = 0;
    for (uint32_t i = 0; i < n; i++)                                          /* equal names are adjacent and in archive order: keep the last */
        if (i + 1 == n || name_cmp(x, a[i], a[i + 1]) != 0) a[u++] = a[i];
    mem_free(b);
    mem_free(x->order);
    x->order = a;
    x->unique = u;
    return 0;
}

/* first position in order whose name is >= path */
uint32_t arc_idx_lower(const ArcIndex *x, const char *path)
{
    uint32_t lo = 0, hi = x->unique;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (str_cmp(x->names + x->items[x->order[mid]].name_off, path) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

const ArcItem *arc_idx_find_raw(const ArcIndex *x, const char *raw, size_t len)
{
    char buf[1024];
    if (len >= sizeof buf - 2) return nullptr;
    bool is_dir, unsafe;
    size_t n = normalise_into(buf, raw, len, &is_dir, &unsafe);
    if (!n) return nullptr;
    if (!is_dir) n--;
    buf[n] = 0;
    return arc_idx_find(x, buf);
}

const ArcItem *arc_idx_find(const ArcIndex *x, const char *path)
{
    uint32_t i = arc_idx_lower(x, path);
    if (i >= x->unique) return nullptr;
    const ArcItem *it = &x->items[x->order[i]];
    return str_cmp(x->names + it->name_off, path) == 0 ? it : nullptr;
}

/* name versus the key prefix + comp + '0' (the first string after every "prefix comp /..." name): the sign of name - key */
static int cmp_after_subtree(const char *n, const char *prefix, size_t plen, const char *comp, size_t clen)
{
    for (size_t k = 0; k < plen; k++)
        if (n[k] != prefix[k]) return (int)(uint8_t)n[k] - (int)(uint8_t)prefix[k];
    for (size_t k = 0; k < clen; k++)
        if (n[plen + k] != comp[k]) return (int)(uint8_t)n[plen + k] - (int)(uint8_t)comp[k];
    return (int)(uint8_t)n[plen + clen] - '0';
}

/* the first position after `from` that is outside the subtree prefix + comp + "/" */
static uint32_t arc_idx_skip(const ArcIndex *x, uint32_t from, const char *prefix, size_t plen, const char *comp, size_t clen)
{
    uint32_t lo = from, hi = x->unique;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cmp_after_subtree(x->names + x->items[x->order[mid]].name_off, prefix, plen, comp, clen) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int arc_idx_list(const ArcIndex *x, const char *prefix, ArcListFn fn, void *ctx)
{
    size_t plen = str_len(prefix);
    uint32_t i = arc_idx_lower(x, prefix);
    while (i < x->unique) {
        const ArcItem *it = &x->items[x->order[i]];
        const char *n = x->names + it->name_off;
        if (!str_starts_with(n, prefix)) break;                               /* left the folder */
        const char *rest = n + plen;
        if (!rest[0] || (it->flags & ARC_F_UNSAFE)) {
            i++;
            continue;
        }
        const char *slash = str_chr(rest, '/');
        int r;
        if (!slash) {
            r = fn(ctx, rest, str_len(rest), false, it);
            i++;
        } else {
            size_t clen = (size_t)(slash - rest);
            r = fn(ctx, rest, clen, true, slash[1] ? nullptr : it);           /* the folder's own entry, when it comes first */
            i = arc_idx_skip(x, i + 1, prefix, plen, rest, clen);
        }
        if (r) return r;
    }
    return 0;
}
