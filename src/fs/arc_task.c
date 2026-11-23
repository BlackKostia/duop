/* src/fs/arc_task.c - the TaskWorker's entry for unpacking and testing archives (freestanding C23).
 *
 * TASK_OP_UNPACK: job->src_dir is the archive file, job->dst the destination folder, job->names "<folder>\0<member>\0..." with job->count
 * members (0: the whole archive), job->join_dst bit 1 = smart unpack. TASK_OP_ARCTEST needs only the archive. The totals and progress
 * go to the shared page (TaskShared, via the fs_progress_* hooks of the engine); the number of members skipped for being unsafe,
 * unsupported or damaged is published for the UI to report after the worker is reaped. */
#include "duop_arcunpack.h"

#define JOB_SMART 2u

intptr_t arc_task_run(const TaskJob *job, TaskShared *shm)
{
    ArcUnpackReport r;
    intptr_t rc;
    if (job->op == TASK_OP_ARCTEST) {
        rc = arc_test(job->src_dir, &r);
    } else {
        ArcUnpackOpts o = {0, (job->join_dst & JOB_SMART) != 0, 0, ARC_BOMB_FLOOR, ARC_BOMB_RATIO};
        rc = arc_unpack(job->src_dir, job->dst, job->names, job->count, &o, &r);
    }
    shm->skipped = r.skipped + r.damaged;
    return rc;
}
