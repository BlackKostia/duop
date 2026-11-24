/* include/duop_arcvfs.h - the archive virtual folder on the new engine (src/fs/archive_vfs.c, arc_source.c, arc_extract.c;
 * docs/ARCHIVE_TOOLS_PLAN.md, phase 1). An archive is memory-mapped, indexed once (ArcIndex) and shown one folder level at a time;
 * a member is extracted by streaming, so nothing is bounded by the size of the archive, of a member or by the number of members. */
#pragma once
#include "duop_arc.h"
#include "duop_arcparse.h"

#ifndef ARC_OPEN_TGZ_MAX
#define ARC_OPEN_TGZ_MAX (512ull << 20)     /* a .tar.gz is unpacked to a temporary file to be indexed: at most this much (the open is synchronous) */
#endif

typedef struct ArcSource {
    const uint8_t *data;                    /* the tar / zip bytes (for .tar.gz: the unpacked tar in an unlinked temporary file) */
    uint64_t len;
    void *map;                              /* what to os_munmap, and its length */
    uint64_t map_len;
    int type;                               /* ARC_TAR / ARC_TGZ / ARC_ZIP / ARC_GZ */
    int64_t mtime;                          /* ARC_GZ: the time in the gzip header (0 when it has none) */
} ArcSource;

typedef struct ArcVfs {                     /* DirModel.arc_ctx while virtual_mode == 2 */
    ArcSource src;
    ArcIndex idx;
    ArcParseInfo info;
    char sub[PATH_MAX];                     /* the folder shown ("" or "a/b/") */
    char orig[PATH_MAX];                    /* the real directory the archive was entered from */
    char file[PATH_MAX];                    /* the archive file */
} ArcVfs;

/* The kind of the archive file at path by its contents, the extension deciding only where they say nothing: ARC_TAR / ARC_TGZ / ARC_ZIP / ARC_GZ
 * (a gzip that is not a tar), ARC_UNSUPPORTED (xz, bzip2, zstd, 7z, rar ... recognised, not implemented), 0 (not an archive). Never blocks (FIFOs). */
int arc_detect_file(const char *path);
/* the index of a source: tar and zip members, or the one member of a ARC_GZ (named after path without .gz) - 0 or the parser's error */
int arc_parse_source(const ArcSource *s, const char *path, ArcIndex *x, ArcParseInfo *info);
/* maps the archive (a .tar.gz / .gz is gunzipped to an unlinked temp file first): 0, -errno, or -EFBIG past ARC_OPEN_TGZ_MAX. Only regular non-empty files. */
int arc_source_open(ArcSource *s, const char *path, int type);
int arc_source_open_max(ArcSource *s, const char *path, int type, uint64_t max_unpacked);   /* the same with another limit for a .tar.gz (the worker's) */
void arc_source_close(ArcSource *s);
/* An extraction in progress: progress() is called after every chunk written (a negative result stops it and becomes the error), buf != NULL
 * collects the data in memory (at most cap bytes) instead of writing to a descriptor (the target of a symlink member). */
typedef struct ArcXfer {
    intptr_t (*progress)(void *ctx, uint64_t n);
    void *ctx;
    uint8_t *buf;
    size_t cap, len;
    intptr_t sink_err;              /* out: the failure of the write or of progress() - the data was fine, the destination is not */
} ArcXfer;
intptr_t arc_extract_to(const ArcSource *s, const ArcItem *it, int fd, ArcXfer *x);   /* as arc_extract_item; x may be NULL */
/* writes the bytes of a regular member to fd, streaming: -EINVAL (not a plain member / damaged), -EACCES (encrypted), -EOPNOTSUPP (links, or a
 * compression method that is not implemented), -EBADMSG (CRC-32 or size mismatch), -EFBIG, or the write error; 0 when complete and verified */
intptr_t arc_extract_item(const ArcSource *s, const ArcItem *it, int fd);
const char *arc_vfs_sub(const DirModel *m);   /* the folder of a model in virtual_mode 2 */
const char *arc_vfs_orig(const DirModel *m);  /* the real directory the archive was entered from */
