/* include/duop_arcunpack_int.h - what the files of the unpack engine share (src/fs/arc_unpack.c, arc_unpack_item.c, arc_path.c, arc_task.c). */
#pragma once
#include "duop_arcunpack.h"
#include "duop_arcvfs.h"

typedef struct ArcUnp {             /* one unpack / test run */
    ArcSource src;
    ArcIndex idx;
    ArcParseInfo info;
    int root;                       /* the destination directory (descriptor) */
    const ArcUnpackOpts *o;
    ArcUnpackReport *r;
    uint32_t *dirs;                 /* folder members made, for their modification times at the end */
    uint32_t ndirs, cap_dirs;
    uint32_t seq;                   /* numbers the temporary files */
    size_t selpfx;                  /* length of the archive folder the selection is relative to: stripped from member names */
} ArcUnp;

/* arc_path.c: below `root`, never through a symlink */
/* The directory holding rel ("a/b/c": a/b), as a descriptor the caller closes; `create` makes missing folders. *leaf points at "c" inside rel.
 * -ELOOP / -ENOTDIR when a component is a symlink or not a folder, -EINVAL for ".", ".." or empty components, -ENAMETOOLONG. */
intptr_t arc_dest_parent(int root, const char *rel, bool create, const char **leaf);
/* a symlink at rel pointing to target stays inside the destination: relative, and ".." never climbs above the root */
bool arc_link_safe(const char *rel, const char *target);

/* arc_unpack_item.c */
/* Creates one member at rel below u->root: 0 when done or skipped (the report says which), a negative error when the whole run must stop
 * (-ECANCELED, a full or failing destination). */
int arc_put_member(ArcUnp *u, const ArcItem *it, const char *rel);
void arc_put_dir_times(ArcUnp *u);          /* the modification times of the folders made, deepest first */
intptr_t arc_xfer_progress(void *ctx, uint64_t n);   /* ArcXfer.progress of the task worker: bytes done, -ECANCELED when asked to stop */
