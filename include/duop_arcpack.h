/* include/duop_arcpack.h - creating archives (src/fs/arc_pack.c, tar_write.c, zip_write.c, deflate.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 6).
 *
 * arc_pack walks the named files and folders below a base folder in sorted order (without recursion, and without ever following a
 * symlink: links are stored as links) and writes a .tar, .tar.gz or .zip into a temporary sibling of dst that is renamed to dst only when
 * the archive is complete (and fsynced); an existing dst is never replaced (-EEXIST). Devices, fifos and sockets, files that cannot be read
 * and names that do not exist are counted as skipped and do not stop the run; the archive being written is never packed into itself. A
 * file that changes size while it is read is stored as the header said (padded with zeros in a tar) and counted as changed.
 * With ARC_PACK_FIXED the result depends only on the contents and names: owners are zero, all times are opts.mtime, modes are normalised, so two
 * runs give the same bytes (reproducible builds). Runs inside the TaskWorker: progress and cancellation (-ECANCELED) via the fs_progress_* hooks. */
#pragma once
#include "duop_abi.h"

#define ARC_PACK_TAR 0
#define ARC_PACK_TGZ 1
#define ARC_PACK_ZIP 2
#define ARC_PACK_FIXED 1u              /* reproducible: uid / gid 0, mtime = opts.mtime, modes 0644 / 0755, no ownership names */

typedef struct ArcPackOpts {
    uint32_t format;                   /* ARC_PACK_* */
    uint32_t level;                    /* 0 stored .. 9 best (6 when out of range); the compression of .tar.gz and .zip */
    uint32_t flags;
    uint32_t pad;
    int64_t mtime;                     /* with ARC_PACK_FIXED */
} ArcPackOpts;

typedef struct ArcPackReport {
    uint64_t files, dirs, links;       /* entries stored */
    uint64_t bytes_in, bytes_out;      /* file data read, archive bytes written */
    uint64_t skipped;                  /* specials, unreadable or vanished names */
    uint64_t changed;                  /* files whose size changed while they were read */
    int64_t first_error;               /* the -errno of the first skipped item (0: none) */
} ArcPackReport;

/* names: "<base folder>\0<name>\0<name>..." (count names, each one name inside the base folder). 0, -ECANCELED, -EEXIST (dst exists), -ENOSPC, other -errno
 * from creating or writing the archive; per-item trouble is in the report. */
intptr_t arc_pack(const char *dst, const char *names, uint32_t count, const ArcPackOpts *o, ArcPackReport *r);
int arc_pack_format(const char *name);                 /* from the extension: .zip, .tar, .tar.gz / .tgz -> ARC_PACK_*; -1 for any other name */
/* test hook: one ustar / PAX header with no data, as the tar writer would write it: its length */
size_t arc_pack_tar_header_test(uint8_t *out, size_t cap, uint64_t size, uint32_t uid, int64_t mtime, char type, uint32_t mode, const char *name, const char *link);
