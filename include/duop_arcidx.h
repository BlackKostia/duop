/* include/duop_arcidx.h - the entry table of an archive (src/fs/arc_index.c; docs/ARCHIVE_TOOLS_PLAN.md, phase 1).
 *
 * The parsers (arc_tar.c, arc_zip.c) add one ArcItem per member; arc_idx_finish sorts the names (bytewise, stable) and keeps the last
 * of equal names - what an extraction would leave on disk. A folder listing then walks the sorted order and jumps over each
 * subtree by binary search, so showing one level costs O(children * log n), however deep or large the archive is. Memory grows
 * by doubling and every growth is checked: a refused add leaves the table exactly as it was. */
#pragma once
#include "duop_abi.h"

#define ARC_F_DIR 1u                /* a directory (the name ends in '/') */
#define ARC_F_SYMLINK 2u            /* link target in the names arena (ArcIndex link) */
#define ARC_F_HARDLINK 4u
#define ARC_F_ENCRYPTED 8u          /* listed, but its data cannot be read */
#define ARC_F_UNSAFE 16u            /* a ".." component: kept for reports, never listed and never unpacked */
#define ARC_F_SPECIAL 32u           /* device, fifo, socket: listed, never created */

#define ARC_IDX_MAX_ITEMS 8388608u  /* 8 Mi members (~450 MB of table): more is refused with -EFBIG */
#define ARC_IDX_MAX_NAME 65535u

typedef struct ArcItem {            /* 56 bytes */
    uint32_t name_off;              /* NUL-terminated normalised path in ArcIndex.names: no leading '/', no "." or empty components */
    uint32_t link_off;              /* link target in names, UINT32_MAX when there is none */
    uint64_t off;                   /* tar: data offset; zip: local header offset */
    uint64_t size;                  /* uncompressed */
    uint64_t comp_size;
    int64_t mtime;                  /* Unix seconds, 0 unknown */
    uint32_t mode;                  /* permission bits (type bits are in flags) */
    uint32_t crc32;                 /* zip: the stored CRC-32 */
    uint16_t method;                /* zip: 0 stored, 8 deflate, 9 deflate64, 12 bzip2, 14 lzma, 93 zstd, 95 xz; tar: 0 */
    uint16_t flags;                 /* ARC_F_* */
    uint32_t pad;
} ArcItem;
_Static_assert(sizeof(ArcItem) == 56, "ArcItem layout");

typedef struct ArcIndex {
    ArcItem *items;
    uint32_t count, cap;            /* members added, slots */
    char *names;                    /* arena of names and link targets */
    uint64_t names_len, names_cap;
    uint32_t *order;                /* after arc_idx_finish: item numbers by name, one per distinct name */
    uint32_t unique;                /* length of order */
    uint32_t unsafe;                /* members added with a ".." component */
} ArcIndex;

typedef int (*ArcListFn)(void *ctx, const char *comp, size_t comp_len, bool is_dir, const ArcItem *item);

void arc_idx_init(ArcIndex *x);
void arc_idx_free(ArcIndex *x);
/* Adds a member (proto supplies everything but the names; ARC_F_DIR / ARC_F_UNSAFE are set here). The name is normalised; a name that
 * normalises to nothing ("./") is skipped with 0. -EINVAL for a NUL inside a name, -ENAMETOOLONG (>= PATH_MAX or ARC_IDX_MAX_NAME),
 * -EFBIG past ARC_IDX_MAX_ITEMS, -ENOMEM: the table is unchanged then. link may be NULL. */
int arc_idx_add(ArcIndex *x, const char *name, size_t name_len, const char *link, size_t link_len, const ArcItem *proto);
int arc_idx_finish(ArcIndex *x);                                       /* 0 or -ENOMEM (the old order stays) */
const ArcItem *arc_idx_find(const ArcIndex *x, const char *path);      /* exact normalised name ("dir/" for a directory), NULL if absent */
/* The children of folder prefix ("" or "a/b/") in name order, each distinct first component once; item is NULL for a folder that exists
 * only through the paths below it. A non-zero result of fn stops the walk and is returned. */
int arc_idx_list(const ArcIndex *x, const char *prefix, ArcListFn fn, void *ctx);
const char *arc_item_name(const ArcIndex *x, const ArcItem *it);
const char *arc_item_link(const ArcIndex *x, const ArcItem *it);       /* NULL without a link target */
