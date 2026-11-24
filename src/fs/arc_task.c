/* src/fs/arc_task.c - the TaskWorker's entry for unpacking, testing and packing archives (freestanding C23).
 *
 * TASK_OP_UNPACK: job->src_dir is the archive file, job->dst the destination folder, job->names "<folder>\0<member>\0..." with job->count
 * members (0: the whole archive), job->join_dst bit 1 = smart unpack. TASK_OP_ARCTEST needs only the archive. The totals and progress
 * go to the shared page (TaskShared, via the fs_progress_* hooks of the engine); the number of members skipped for being unsafe,
 * unsupported or damaged is published for the UI to report after the worker is reaped. TASK_OP_PACK: job->src_dir is the base folder, job->names the
 * entries, job->dst the archive to create, job->join_dst its format (ARC_PACK_*); see arc_pack.c. */
#include "duop_arcpack.h"
#include "duop_arcunpack.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"

#define JOB_SMART 2u

intptr_t arc_task_run(const TaskJob *job, TaskShared *shm)
{
    ArcUnpackReport r;
    intptr_t rc;
    if (job->op == TASK_OP_PACK) {                                   /* names for the engine: "<base folder>\0<entry>\0..." */
        size_t bl = str_len(job->src_dir) + 1;
        char *names = mem_alloc(bl + job->names_len + 1);
        if (!names) return -ENOMEM;
        __builtin_memcpy(names, job->src_dir, bl);
        __builtin_memcpy(names + bl, job->names, job->names_len);
        ArcPackOpts po = {job->join_dst & 3, 6, 0, 0, 0};
        ArcPackReport pr;
        rc = arc_pack(job->dst, names, job->count, &po, &pr);
        shm->skipped = pr.skipped;
        mem_free(names);
        return rc;
    }
    if (job->op == TASK_OP_ARCTEST) {
        rc = arc_test(job->src_dir, &r);
    } else {
        ArcUnpackOpts o = {0, (job->join_dst & JOB_SMART) != 0, 0, ARC_BOMB_FLOOR, ARC_BOMB_RATIO};
        rc = arc_unpack(job->src_dir, job->dst, job->names, job->count, &o, &r);
    }
    shm->skipped = r.skipped + r.damaged;
    return rc;
}
