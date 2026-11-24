/* include/duop_task.h - C view of the background task worker (src/fs/task*.c): the shared progress page, the job and the API.
 * Layouts mirror include/types.inc; tests/test_abi_layouts.py compares every offset with the assembler's. */
#pragma once
#include "duop_fs.h"

#define TASK_OP_COPY 1
#define TASK_OP_MOVE 2
#define TASK_OP_DELETE 3
#define TASK_OP_UNPACK 4         /* archive members (job: src_dir = archive, names = [sub, member keys...]) into dst */
#define TASK_OP_ARCTEST 5        /* every member decoded and verified, nothing written */
#define TASK_OP_PACK 6           /* files into an archive (job: src_dir = base folder, names = the entries, dst = the archive, join_dst = ARC_PACK_* format) */
#define TASK_PHASE_SCAN 1
#define TASK_PHASE_RUN 2
#define TASK_PHASE_DONE 3
#define TASK_SHM_SIZE 4096         /* one shared page for TaskShared */
#define TASK_NAME_MAX 256          /* == NAME_MAX of the layouts */

/* MAP_SHARED page written by the worker, read by the UI; counters are naturally aligned (atomic), cur_name uses a seqlock */
typedef struct TaskShared {
    uint64_t total_bytes, total_files;   /* discovered by the scan phase */
    uint64_t done_bytes, done_files;     /* processed so far */
    int64_t result;                      /* final 0 / -errno (valid once phase == TASK_PHASE_DONE) */
    uint32_t seq;                        /* seqlock of cur_name (odd = writer active) */
    uint32_t phase;                      /* TASK_PHASE_* */
    uint32_t cancel;                     /* 1: the UI asks for a cooperative cancel */
    uint32_t conflict_state;             /* 0 none, 1 worker waiting, 2 UI decision ready */
    uint32_t conflict_action, conflict_all;
    uint64_t conflict_src_sz, conflict_dst_sz;
    uint64_t skipped;                    /* unpack / test: members left out or failed, the worker went on */
    char cur_name[TASK_NAME_MAX];
    char conflict_name[TASK_NAME_MAX];
} TaskShared;
_Static_assert(sizeof(TaskShared) <= TASK_SHM_SIZE, "TaskShared must fit in one shared page");

/* heap job handed to fs_task_start: count NUL-separated names after the fixed part */
typedef struct TaskJob {
    uint64_t names_len;                  /* bytes used in names[] */
    uint32_t op, count;
    uint32_t join_dst;                   /* 1: dst/<name>, 0: dst verbatim (single item); unpack: bit 1 = smart unpack (see arc_task.c) */
    uint32_t pad;
    char src_dir[PATH_MAX];
    char dst[PATH_MAX];
    char names[];
} TaskJob;

/* the UI's consistent snapshot of the running task */
typedef struct TaskSnap {
    uint64_t total_bytes, total_files, done_bytes, done_files;
    uint64_t elapsed_ms;                 /* active time (pauses excluded) */
    uint64_t speed_bps;                  /* bytes per second (0 = unknown) */
    uint64_t eta_sec;                    /* seconds remaining (UINT64_MAX = unknown) */
    uint32_t op, phase, paused;
    uint32_t percent;                    /* 0..100 */
    uint32_t canceling, pad;
    char cur_name[TASK_NAME_MAX];
} TaskSnap;

extern TaskShared *g_fs_progress;        /* worker: the page receiving the fs_progress_* hooks (UI: NULL) */
extern int g_measure_bytes;              /* worker: the scan needs regular-file sizes (copy / move) */
extern int g_task_parent_pid;            /* worker: the UI process (a dead parent cancels) */
extern int64_t g_task_start_ms;
extern int g_task_wake_fd, g_task_op;
extern int g_task_pid, g_task_paused;    /* task_control.s */
extern int64_t g_task_pause_ms, g_task_cancel_at;
extern TaskShared *g_task_shm;
extern uint64_t g_task_skipped;          /* UI: TaskShared.skipped of the worker reaped last (valid after fs_task_poll returned 1) */

/* worker-side progress hooks (task.c): no-ops in the UI process */
void fs_progress_begin(const char *path);                      /* basename -> cur_name under the seqlock */
int task_measure(const char *path);                            /* adds the tree's entries / bytes to the totals: 0 or -ECANCELED */

/* the UI side (task_control.c) */
int fs_task_poll(int64_t *out_result);                         /* 1 once reaped (result stored), else 0 */
void fs_task_snapshot(TaskSnap *out);
int fs_task_toggle_pause(void);                                /* the new paused state */
void fs_task_cancel(void);
void fs_task_shutdown(void);                                   /* cancel, reap, SIGKILL after the grace time */

/* conflicts (task_conflict.c) */
#define CONFLICT_ACT_OVERWRITE 1
#define CONFLICT_ACT_SKIP 2
#define CONFLICT_ACT_RENAME 3
extern int g_copy_had_skip;                                    /* a copy skipped an item: a move keeps its source */
void task_make_unique_name(int dfd, const char *orig, char *out);        /* "<stem>_<n><ext>" of orig that dfd does not hold yet (PATH_MAX buffer) */
intptr_t fs_task_resolve_conflict(int dst_dfd, const char *dst_rel, uint64_t src_sz, uint64_t dst_sz, char *out_name);
int fs_task_check_conflict(char *out_name, uint64_t *out_src_sz, uint64_t *out_dst_sz);   /* UI: 1 while the worker waits */
void fs_task_reply_conflict(int action, int for_all);
FoOpenResult fo_resolve_eexist_open(int dst_dfd, const char *dst_rel, const FileStat *src_st, char *out_ren);
intptr_t fo_move_check_conflict(const char *src, const char *dst, char *out_ren);

/* the worker (task_worker.c) */
extern intptr_t (*g_task_run_hook)(const TaskJob *job, TaskShared *shm);   /* runs the ops above TASK_OP_DELETE (archive unpack / test, set by the app) in the worker */
intptr_t fs_task_start(const TaskJob *job);                    /* forks the worker: 0 (the caller may free job), -EBUSY, -errno */
int fs_task_is_active(void);
int fs_task_wake_fd(void);                                     /* read end that hangs up when the worker exits, or -1 */

