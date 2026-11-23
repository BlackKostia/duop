/* src/fs/task_control.c - the UI side of the background task: reaping, progress snapshots, pause, cancel and shutdown
 * (freestanding C23; replaces task_control.s, kept as tests/harness/task_control_reference.s).
 *
 * Pause is SIGSTOP / SIGCONT (the paused time is excluded from speed and ETA); cancel sets the cooperative flag and sends SIGTERM,
 * which the worker handles without SA_RESTART, so a blocking open() / read() returns -EINTR; while a cancel is pending every poll
 * re-sends it. On exit the worker is cancelled and, after TASK_KILL_GRACE_MS, killed: no orphan keeps writing files. */
#include "duop_task.h"
#include "duop_pal.h"
#include "duop_string.h"

#define TASK_KILL_GRACE_MS 3000

TaskShared *g_task_shm;            /* the page of the running worker */
uint64_t g_task_skipped;           /* its `skipped` counter once reaped */
int64_t g_task_pause_ms;           /* accumulated paused time */
static int64_t g_task_pause_at;    /* start of the current pause */
int64_t g_task_cancel_at;          /* when the cancel was requested (0: none) */
int g_task_pid, g_task_paused;

/* the resources of a reaped worker */
static void task_release(void)
{
    (void)os_close(g_task_wake_fd);
    (void)os_munmap_raw(g_task_shm, TASK_SHM_SIZE);
    g_task_pid = 0;
    g_task_shm = nullptr;
    g_task_paused = 0;
    g_task_cancel_at = 0;
    g_task_wake_fd = -1;
}

/* Non-blocking: 1 with 0 / -errno / -ECANCELED in *out_result once the worker is reaped (-EIO: it died without a result), else 0.
 * While a cancel is pending SIGTERM goes again: the worker may have checked the flag just before a blocking system call. */
int fs_task_poll(int64_t *out_result)
{
    if (!g_task_pid) return 0;
    int status;
    intptr_t r;
    while ((r = os_wait4(g_task_pid, &status, WNOHANG, nullptr)) == -EINTR) {}
    if (r > 0 || r == -ECHILD) {
        int64_t res = __atomic_load_n(&g_task_shm->phase, __ATOMIC_ACQUIRE) == TASK_PHASE_DONE ? g_task_shm->result : -EIO;
        g_task_skipped = g_task_shm->skipped;
        task_release();
        *out_result = res;
        return 1;
    }
    if (g_task_cancel_at) (void)os_kill(g_task_pid, SIGTERM);
    return 0;
}

/* a * b / c for a quotient that fits 64 bits (the caller checks that the high half of a * b is below c) */
static uint64_t tc_muldiv(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    uint64_t lo = (p00 & 0xFFFFFFFFu) | (mid << 32), hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    uint64_t q = 0;                                                    /* restoring division of hi:lo by c, hi < c */
    for (int i = 63; i >= 0; i--) {
        bool carry = hi >> 63;
        hi = hi << 1 | lo >> 63;
        lo <<= 1;
        if (carry || hi >= c) { hi -= c; q |= 1ull << i; }
    }
    return q;
}

/* the high 64 bits of a * b */
static uint64_t tc_mulhi(uint64_t a, uint64_t b)
{
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    return a1 * b1 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static void snap_name(TaskSnap *out, const TaskShared *s)       /* seqlock read of the current item (bounded retries) */
{
    for (int tries = 8; tries > 0; tries--) {
        uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        mem_copy(out->cur_name, s->cur_name, TASK_NAME_MAX);
        if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) == seq) break;
    }
    out->cur_name[TASK_NAME_MAX - 1] = 0;
}

/* counters, percent, speed and ETA (pauses excluded) and a torn-free name; untouched without a running task */
void fs_task_snapshot(TaskSnap *out)
{
    const TaskShared *s = g_task_shm;
    if (!s) return;
    out->total_bytes = s->total_bytes;
    out->total_files = s->total_files;
    out->done_bytes = s->done_bytes;
    out->done_files = s->done_files;
    out->phase = s->phase;
    out->op = (uint32_t)g_task_op;
    out->paused = (uint32_t)g_task_paused;
    out->canceling = g_task_cancel_at != 0;
    snap_name(out, s);
    int64_t now = os_clock_ms(), el = now - g_task_start_ms - g_task_pause_ms;
    if (g_task_paused) el -= now - g_task_pause_at;
    uint64_t elapsed = el < 0 ? 0 : (uint64_t)el;
    out->elapsed_ms = elapsed;
    bool by_bytes = out->op != TASK_OP_DELETE && out->total_bytes;    /* bytes for copy / move when known, else entries */
    uint64_t done = by_bytes ? out->done_bytes : out->done_files, total = by_bytes ? out->total_bytes : out->total_files;
    if (done > total) done = total;
    out->percent = total && out->phase != TASK_PHASE_SCAN ? (uint32_t)tc_muldiv(done, 100, total) : 0;
    out->speed_bps = 0;
    out->eta_sec = UINT64_MAX;
    if (elapsed < 500) return;                                         /* speed and ETA once 500 ms of active time elapsed */
    if (tc_mulhi(out->done_bytes, 1000) < elapsed) out->speed_bps = tc_muldiv(out->done_bytes, 1000, elapsed);
    if (out->phase != TASK_PHASE_RUN || !done) return;
    if (tc_mulhi(total - done, elapsed) >= done) return;
    out->eta_sec = (tc_muldiv(total - done, elapsed, done) + 999) / 1000;
}

static void task_resume(int64_t now)                               /* SIGCONT a paused worker, account the pause */
{
    if (!g_task_paused) return;
    g_task_pause_ms += now - g_task_pause_at;
    g_task_paused = 0;
    (void)os_kill(g_task_pid, SIGCONT);
}

/* the new paused state (a cancelling worker must keep running to stop) */
int fs_task_toggle_pause(void)
{
    if (g_task_pid && !g_task_cancel_at) {
        int64_t now = os_clock_ms();
        if (g_task_paused) task_resume(now);
        else if (os_kill(g_task_pid, SIGSTOP) >= 0) {
            g_task_pause_at = now;
            g_task_paused = 1;
        }
    }
    return g_task_paused;
}

/* the cooperative flag, SIGCONT (a paused worker must see it) and SIGTERM (interrupts blocking I/O) */
void fs_task_cancel(void)
{
    if (!g_task_pid) return;
    __atomic_store_n(&g_task_shm->cancel, 1, __ATOMIC_RELEASE);
    int64_t now = os_clock_ms();
    if (!g_task_cancel_at) g_task_cancel_at = now;
    task_resume(now);
    (void)os_kill(g_task_pid, SIGTERM);
}

/* cancels and reaps the worker before the UI exits; SIGKILL after TASK_KILL_GRACE_MS */
void fs_task_shutdown(void)
{
    if (!g_task_pid) return;
    fs_task_cancel();
    uint64_t deadline = (uint64_t)os_clock_ms() + TASK_KILL_GRACE_MS;
    for (;;) {
        int64_t res;
        if (fs_task_poll(&res)) return;
        if ((uint64_t)os_clock_ms() >= deadline) {
            (void)os_kill(g_task_pid, SIGKILL);
            deadline = UINT64_MAX;
        }
        (void)os_poll_in(g_task_wake_fd, 50);                          /* returns early when the worker exits (pipe hang-up) */
    }
}
