/* include/duop_arcunpack.h - unpacking and testing archives (src/fs/arc_unpack.c, arc_unpack_item.c, arc_path.c, arc_task.c;
 * docs/ARCHIVE_TOOLS_PLAN.md, phase 2). The engine runs inside the TaskWorker (a forked process: progress and cancellation go through
 * the shared page, the UI never blocks) but is a plain function, so it is tested without a UI.
 *
 * Safety (the archive is untrusted): every file is created below the destination through directory descriptors opened with
 * O_NOFOLLOW (arc_path.c), so no symlink - whether created by the archive itself or already there - can redirect a write; members
 * with ".." components, devices / fifos / sockets and absolute or escaping symlink targets are never created (counted as skipped);
 * a hard link is a copy of its target member, never a link to a file outside the archive; file modes lose setuid / setgid / sticky
 * and honour the umask; each file is written to a temporary sibling, verified (CRC-32 / size), and only then renamed to its name, so
 * a crash or a damaged member leaves no partial file under a real name. */
#pragma once
#include "duop_task.h"

typedef struct ArcUnpackOpts {
    uint32_t on_conflict;           /* 0: ask the user through the task page (overwrite when run without one); else CONFLICT_ACT_* */
    uint32_t smart_dir;             /* 1: a whole archive with several top-level entries goes into <dst>/<archive name>/ (no tarbomb) */
    uint64_t max_bytes;             /* refuse (-EFBIG) when the members add up to more; 0 = no cap */
    uint64_t bomb_floor, bomb_ratio;/* refuse (-EFBIG) above bomb_floor unpacked bytes when that is more than bomb_ratio times the size of the archive file; 0 = off */
} ArcUnpackOpts;

#define ARC_BOMB_FLOOR (1ull << 30)  /* the UI's defaults: more than 1 GiB unpacked ... */
#define ARC_BOMB_RATIO 1000ull       /* ... out of an archive 1000 times smaller is a decompression bomb */

typedef struct ArcUnpackReport {
    uint64_t files, dirs, links;    /* created (a hard link counts as a file) */
    uint64_t bytes;                 /* data written */
    uint64_t skipped;               /* members not created: unsafe name, device / fifo, escaping link, encrypted, unsupported or damaged data */
    uint64_t kept;                  /* members left alone because the user (or the policy) chose Skip */
    int64_t first_error;            /* the -errno of the first skipped member (0: none) */
    uint32_t damaged;               /* the archive itself is damaged or ends early: what could be read was used */
    uint32_t pad;
} ArcUnpackReport;

/* names: "<folder>\0<member>\0<member>..." (count members), the folder being the archive folder the members are listed in ("" or "a/b/");
 * count 0 unpacks everything. 0, -ECANCELED, -ENOSPC (not enough room, or the write failed), -EFBIG (cap / bomb), -ENOTDIR (destination),
 * -errno from opening the archive; per-member trouble is in the report, not in the result. */
intptr_t arc_unpack(const char *archive, const char *dst, const char *names, uint32_t count, const ArcUnpackOpts *o, ArcUnpackReport *r);
/* decodes and verifies every regular member, writes nothing: report.files / bytes = members and bytes checked, skipped = failed */
intptr_t arc_test(const char *archive, ArcUnpackReport *r);
/* TaskWorker entry for TASK_OP_UNPACK / TASK_OP_ARCTEST: sizes the job into the shared page, runs it; the result for task_worker_exit */
intptr_t arc_task_run(const TaskJob *job, TaskShared *shm);
