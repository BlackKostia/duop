/* include/duop_arc.h - C view of the archive layer (src/fs/archive_*.c): the in-memory .tar / .tar.gz / .zip index and its
 * virtual-folder view in a DirModel. The layouts are the assembler's (pkg_archive.s and pkg_manager.s still use them). */
#pragma once
#include "duop_fs.h"

#define ARC_MAX_FILE 4194304         /* at most 4 MiB of an archive are read */
#define ARC_MAX_ENTRIES 1024
#define ARC_MAX_UNCOMP 67108864      /* 64 MiB: the largest ZIP entry inflated */
#define ARC_TAR 1
#define ARC_TGZ 2
#define ARC_ZIP 3

typedef struct ArcEntry {            /* 288 bytes */
    char name[256];
    uint64_t off;                    /* tar: data offset; zip: local header offset */
    uint64_t size;                   /* uncompressed */
    uint64_t comp_size;
    uint32_t method;                 /* 0 stored, 8 deflate */
    uint32_t is_dir;
} ArcEntry;
_Static_assert(sizeof(ArcEntry) == 288, "AENT_SIZE of the assembler");

typedef struct ArcCtx {
    uint32_t type;                   /* ARC_TAR / ARC_TGZ / ARC_ZIP */
    uint32_t count;
    uint8_t *raw;                    /* the archive (a .tar.gz already gunzipped) */
    uint64_t raw_len;
    ArcEntry *entries;               /* ARC_MAX_ENTRIES slots */
    char sub[PATH_MAX];              /* the folder shown inside the archive ("" or "a/b/") */
    char orig[PATH_MAX];             /* the real directory the archive was entered from */
    char file[PATH_MAX];             /* the archive file */
} ArcCtx;
_Static_assert(sizeof(ArcCtx) == 32 + 3 * PATH_MAX, "ACTX_SIZE of the assembler");

/* parsing (archive_parse.c): untrusted input, every offset checked without overflow */
int arc_is_archive(const char *name);                          /* by extension: ARC_TAR / ARC_TGZ / ARC_ZIP, 0 */
void arc_parse_tar(ArcCtx *ctx);
void arc_parse_zip(ArcCtx *ctx);
void *arc_ungzip(const uint8_t *src, uint64_t src_len, uint64_t *out_len);   /* mem_alloc'd data or NULL */

/* the virtual folder (archive_vfs.c on arc_source.c / arc_index.c / arc_tar.c / arc_zip.c: include/duop_arcvfs.h). ArcEntry / ArcCtx and the
 * parsers above are the package manager's (pkg_archive.c) until it moves to the new engine. */
void arc_populate(DirModel *m);                                /* the entries of ctx->sub as a listing */
void arc_free_ctx(DirModel *m);
int arc_nav_up_model(DirModel *m);                             /* -1: not in an archive; else the cursor */
int arc_enter_model(DirModel *m, const FileEntry *ent);        /* 0 no, 1 entered, 2 "..", 3 entered but the archive is damaged / incomplete */
intptr_t arc_extract_entry(DirModel *m, const FileEntry *ent, char *out_tmp_path);   /* the member into a temp file: 0, or -errno (EACCES encrypted, EOPNOTSUPP method, EBADMSG checksum) */

int ds_append_entry(DirModel *m, const char *rel_name, const FileStat *st);   /* dir_search.c */
int ds_append_parent(DirModel *m);
