/* include/duop_arcvfs.h - the archive virtual folder on the new engine (src/fs/archive_vfs.c, arc_source.c, arc_extract.c;
 * docs/ARCHIVE_TOOLS_PLAN.md, phase 1). An archive is memory-mapped, indexed once (ArcIndex) and shown one folder level at a time;
 * a member is extracted by streaming, so nothing is bounded by the size of the archive, of a member or by the number of members. */
#pragma once
#include "duop_arc.h"
#include "duop_arcparse.h"

#define ARC_OPEN_TGZ_MAX (512ull << 20)     /* a .tar.gz is unpacked to a temporary file to be indexed: at most this much (the open is synchronous) */

typedef struct ArcSource {
    const uint8_t *data;                    /* the tar / zip bytes (for .tar.gz: the unpacked tar in an unlinked temporary file) */
    uint64_t len;
    void *map;                              /* what to os_munmap, and its length */
    uint64_t map_len;
    int type;                               /* ARC_TAR / ARC_TGZ / ARC_ZIP */
} ArcSource;

typedef struct ArcVfs {                     /* DirModel.arc_ctx while virtual_mode == 2 */
    ArcSource src;
    ArcIndex idx;
    ArcParseInfo info;
    char sub[PATH_MAX];                     /* the folder shown ("" or "a/b/") */
    char orig[PATH_MAX];                    /* the real directory the archive was entered from */
    char file[PATH_MAX];                    /* the archive file */
} ArcVfs;

/* maps the archive (a .tar.gz is gunzipped to an unlinked temp file first): 0, -errno, or -EFBIG past ARC_OPEN_TGZ_MAX. Only regular non-empty files. */
int arc_source_open(ArcSource *s, const char *path, int type);
void arc_source_close(ArcSource *s);
/* writes the bytes of a regular member to fd, streaming: -EINVAL (not a plain member / damaged), -EACCES (encrypted), -EOPNOTSUPP (links, or a
 * compression method that is not implemented), -EBADMSG (CRC-32 or size mismatch), -EFBIG, or the write error; 0 when complete and verified */
intptr_t arc_extract_item(const ArcSource *s, const ArcItem *it, int fd);
const char *arc_vfs_sub(const DirModel *m);   /* the folder of a model in virtual_mode 2 */
