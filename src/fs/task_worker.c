/* src/fs/task_worker.c - the fork-isolated background worker for copy / move / delete (freestanding C23; replaces task_worker.s,
 * kept as tests/harness/task_worker_reference.s).
 *
 * fs_task_start forks one worker per batch. The worker owns a private copy of the address space (no threads: the allocator and the
 * global buffers stay race-free) and publishes progress into a MAP_SHARED page; a CLOEXEC pipe whose write end only the worker
 * holds hangs up the instant it exits, which wakes the UI event loop. Phase SCAN measures every item (live totals), phase RUN
 * executes them in order and stops at the first failure; after each item the counters are resynchronised to the scan's exact
 * cumulative totals. A move within one file system is a rename: its tree is not scanned. */
#include "duop_task.h"
#include "duop_pal.h"
#include "duop_string.h"

int64_t g_task_start_ms;
intptr_t (*g_task_run_hook)(const TaskJob *job, TaskShared *shm);
int g_task_wake_fd, g_task_op, g_task_parent_pid;
static int g_task_pipe[2];

/* worker-private (one process per job): the paths of the current item */
static char g_tw_src[PATH_MAX], g_tw_dst[PATH_MAX];

typedef struct TwCum { uint64_t bytes, files; } TwCum;        /* totals after the scan of item i */

[[noreturn]] static void task_worker_exit(TaskShared *shm, intptr_t res)
{
    if (res != -ECANCELED && fs_progress_cancelled()) res = -ECANCELED;
    shm->result = res;
    __atomic_store_n(&shm->phase, TASK_PHASE_DONE, __ATOMIC_RELEASE);
    os_exit(0);                                                       /* fault-checked: worker process exit */
}

/* MOVE: the device of the destination directory (an item on the same device is renamed, not scanned), or UINT64_MAX */
static uint64_t tw_move_dev(const TaskJob *job)
{
    if (job->op != TASK_OP_MOVE) return UINT64_MAX;
    const char *dir = job->dst;
    if (!job->join_dst) {
        path_dirname(job->dst, g_tw_dst, PATH_MAX);
        dir = g_tw_dst;
    }
    FileStat st;
    return os_statat(AT_FDCWD, dir, &st, 1) < 0 ? UINT64_MAX : st.dev;
}

static intptr_t tw_scan(const TaskJob *job, TaskShared *shm, TwCum *cum)
{
    uint64_t dev = tw_move_dev(job);
    shm->phase = TASK_PHASE_SCAN;
    const char *name = job->names;
    for (uint32_t i = 0; i < job->count; i++, name += str_len(name) + 1) {
        if (path_join(g_tw_src, job->src_dir, name, PATH_MAX) < 0) return -ENAMETOOLONG;
        FileStat st;
        if (dev != UINT64_MAX && os_statat(AT_FDCWD, g_tw_src, &st, 0) >= 0 && st.dev == dev) shm->total_files++;   /* a rename */
        else {
            int r = task_measure(g_tw_src);
            if (r < 0) return r;
        }
        cum[i] = (TwCum){shm->total_bytes, shm->total_files};
    }
    return 0;
}

static intptr_t tw_item(const TaskJob *job, const char *name)
{
    int64_t r = path_join(g_tw_src, job->src_dir, name, PATH_MAX);  /* path-checked */
    if (r < 0) return r;
    if (job->join_dst) {
        if ((r = path_join(g_tw_dst, job->dst, name, PATH_MAX)) < 0) return r;   /* path-checked */
    } else {
        (void)str_copy(g_tw_dst, job->dst, PATH_MAX);
    }
    fs_progress_begin(g_tw_src);
    if (job->op == TASK_OP_COPY) return fs_copy_recursive(g_tw_src, g_tw_dst);
    if (job->op == TASK_OP_MOVE) return fs_move_recursive(g_tw_src, g_tw_dst);
    return fs_delete_recursive(g_tw_src);
}

[[noreturn]] static void task_worker_main(const TaskJob *job, TaskShared *shm)
{
    g_fs_progress = shm;
    os_mark_child_forked();
    os_uring_atfork_reset();
    os_signals_worker_mode();
    (void)os_set_pdeathsig(g_task_parent_pid, SIGTERM);
    os_raise_nofile(4096);
    g_measure_bytes = job->op != TASK_OP_DELETE;
    if (fs_progress_cancelled()) task_worker_exit(shm, -ECANCELED);
    if (job->op > TASK_OP_DELETE) task_worker_exit(shm, g_task_run_hook ? g_task_run_hook(job, shm) : -ENOSYS);   /* archive unpack / test */
    TwCum *cum = mem_alloc((size_t)job->count * sizeof(TwCum) + 16);
    if (!cum) task_worker_exit(shm, -ENOMEM);
    intptr_t r = tw_scan(job, shm, cum);
    if (r < 0) task_worker_exit(shm, r);
    shm->phase = TASK_PHASE_RUN;
    const char *name = job->names;
    for (uint32_t i = 0; i < job->count; i++, name += str_len(name) + 1) {
        if (fs_progress_cancelled()) task_worker_exit(shm, -ECANCELED);
        if ((r = tw_item(job, name)) < 0) task_worker_exit(shm, r);
        shm->done_bytes = cum[i].bytes;                               /* exact resync after each item */
        shm->done_files = cum[i].files;
    }
    task_worker_exit(shm, 0);
}

intptr_t fs_task_start(const TaskJob *job)
{
    if (g_task_pid) return -EBUSY;
    intptr_t m = os_mmap(nullptr, TASK_SHM_SIZE, PROT_RW, MAP_SHARED_ANON, -1, 0);
    if (m < 0 && m >= -4095) return m;
    TaskShared *shm = (TaskShared *)m;                                /* zero-filled shared page */
    shm->phase = TASK_PHASE_SCAN;
    intptr_t r = os_pipe_cloexec(g_task_pipe);
    if (r < 0) goto unmap;
    g_task_op = (int)job->op;
    g_task_paused = 0;
    g_task_pause_ms = g_task_cancel_at = 0;
    g_task_start_ms = os_clock_ms();
    g_task_parent_pid = (int)os_getpid();
    intptr_t pid = os_fork();
    if (pid == 0) {
        (void)os_close(g_task_pipe[0]);                               /* the worker keeps only the write end (closed by exit) */
        task_worker_main(job, shm);
    }
    if (pid < 0) {
        r = pid;
        (void)os_close(g_task_pipe[0]);
        (void)os_close(g_task_pipe[1]);
        goto unmap;
    }
    g_task_pid = (int)pid;
    g_task_shm = shm;
    (void)os_close(g_task_pipe[1]);                                   /* only the worker holds the write end */
    g_task_wake_fd = g_task_pipe[0];
    return 0;
unmap:
    (void)os_munmap_raw(shm, TASK_SHM_SIZE);
    return r;
}

int fs_task_is_active(void) { return g_task_pid != 0; }

int fs_task_wake_fd(void) { return g_task_pid ? g_task_wake_fd : -1; }
