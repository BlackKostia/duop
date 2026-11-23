/* src/fs/task_conflict.c - conflicts of the background copy / move: detection, the worker <-> UI handshake and unique names
 * (freestanding C23; replaces task_conflict.s, kept as tests/harness/task_conflict_reference.s).
 *
 * When a target exists the worker publishes the name and both sizes in TaskShared, sets conflict_state = 1 and waits (polling,
 * cancelable, aborted when the UI dies) until the UI answers with conflict_state = 2: overwrite, skip or rename (to a free
 * "<stem>_<n><ext>"), optionally for all remaining conflicts. The UI process itself always overwrites (it asked beforehand). */
#include "duop_task.h"
#include "duop_pal.h"
#include "duop_string.h"

#define FO_SKIP (-2)
#define FO_SAME_FILE (-9999)
#define UNIQUE_TRIES 1088            /* _1 .. _1023, then _x<seq> */

static uint32_t g_conflict_all_act;  /* worker: the action chosen "for all" (0: ask) */
static uint32_t g_cfl_seq;
int g_copy_had_skip;

/* "<stem>_<n><ext>" of orig (the extension: the last '.' of the last component that is not its first byte) that dfd does not hold
 * yet, into out (PATH_MAX); after UNIQUE_TRIES failures out keeps the last candidate */
void task_make_unique_name(int dfd, const char *orig, char *out)
{
    const char *dot = nullptr, *p = orig;
    for (; *p; p++) {
        if (*p == '/') dot = nullptr;
        else if (*p == '.') dot = p;
    }
    if (!dot || dot == orig || dot[-1] == '/') dot = p;               /* no extension: append at the end */
    size_t stem = (size_t)(dot - orig);
    if (stem > PATH_MAX - 16) stem = PATH_MAX - 16;
    for (uint32_t n = 1; n < UNIQUE_TRIES; n++) {
        mem_copy(out, orig, stem);
        size_t k = stem;
        out[k++] = '_';
        uint32_t v = n;
        if (n >= 1024) {
            out[k++] = 'x';
            v = __atomic_add_fetch(&g_cfl_seq, 1, __ATOMIC_SEQ_CST);
        }
        k += u64_to_dec(v, out + k);
        (void)str_copy(out + k, dot, PATH_MAX - k);
        FileStat st;
        if (os_statat(dfd, out, &st, 0) == -ENOENT) return;
    }
}

/* 0 once the UI answered, -1 on cancel or when the UI is gone */
static int wait_conflict_reply(const TaskShared *s)
{
    for (;;) {
        if (__atomic_load_n(&s->cancel, __ATOMIC_ACQUIRE)) return -1;
        if (__atomic_load_n(&s->conflict_state, __ATOMIC_ACQUIRE) == 2) return 0;
        if (g_task_parent_pid && !os_parent_alive(g_task_parent_pid)) return -1;
        (void)os_poll_in(-1, 50);
    }
}

/* CONFLICT_ACT_* for an existing dst_rel (RENAME: the free name in out_name), or -ECANCELED */
intptr_t fs_task_resolve_conflict(int dst_dfd, const char *dst_rel, uint64_t src_sz, uint64_t dst_sz, char *out_name)
{
    TaskShared *s = g_fs_progress;
    if (!s) return CONFLICT_ACT_OVERWRITE;
    uint32_t act = g_conflict_all_act;
    if (!act) {
        s->conflict_src_sz = src_sz;
        s->conflict_dst_sz = dst_sz;
        (void)str_copy(s->conflict_name, path_basename(dst_rel), TASK_NAME_MAX);
        s->conflict_action = 0;
        s->conflict_all = 0;
        __atomic_store_n(&s->conflict_state, 1, __ATOMIC_RELEASE);
        if (wait_conflict_reply(s) < 0) {
            s->conflict_state = 0;
            return -ECANCELED;
        }
        act = s->conflict_action;
        uint32_t all = s->conflict_all;
        s->conflict_state = 0;
        if (all) g_conflict_all_act = act;
    }
    if (act == CONFLICT_ACT_RENAME) task_make_unique_name(dst_dfd, dst_rel, out_name);
    return act;
}

/* UI: 1 while the worker waits for an answer (name and sizes copied out where asked), else 0 */
int fs_task_check_conflict(char *out_name, uint64_t *out_src_sz, uint64_t *out_dst_sz)
{
    const TaskShared *s = g_task_shm;
    if (!s || __atomic_load_n(&s->conflict_state, __ATOMIC_ACQUIRE) != 1) return 0;
    if (out_src_sz) *out_src_sz = s->conflict_src_sz;
    if (out_dst_sz) *out_dst_sz = s->conflict_dst_sz;
    if (out_name) (void)str_copy(out_name, s->conflict_name, TASK_NAME_MAX);
    return 1;
}

void fs_task_reply_conflict(int action, int for_all)
{
    TaskShared *s = g_task_shm;
    if (!s) return;
    s->conflict_action = (uint32_t)action;
    s->conflict_all = (uint32_t)for_all;
    __atomic_store_n(&s->conflict_state, 2, __ATOMIC_RELEASE);
}

/* The copy engine met an existing target: the fd to write (used = 1: a temp file the caller renames over the target), -2 = skip,
 * -9999 = the target is the source itself, or -errno. */
FoOpenResult fo_resolve_eexist_open(int dst_dfd, const char *dst_rel, const FileStat *src_st, char *out_ren)
{
    FileStat dst = {0};                                                /* vanished meanwhile: size 0, mode 0 (the asm read stale stack) */
    bool have = os_statat(dst_dfd, dst_rel, &dst, 0) >= 0;
    if (have && src_st->dev == dst.dev && src_st->ino == dst.ino) return (FoOpenResult){FO_SAME_FILE, 0};
    intptr_t act = fs_task_resolve_conflict(dst_dfd, dst_rel, (uint64_t)src_st->size, (uint64_t)dst.size, out_ren);
    if (act < 0) return (FoOpenResult){act, 0};
    if ((uint32_t)act == CONFLICT_ACT_SKIP) {
        g_copy_had_skip = 1;
        return (FoOpenResult){FO_SKIP, 0};
    }
    uint32_t mode = (src_st->mode & 0777) | 0600;
    if ((uint32_t)act == CONFLICT_ACT_RENAME)                          /* out_ren is the final name: nothing to rename later */
        return (FoOpenResult){os_openat(dst_dfd, out_ren, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode), 0};
    return fo_open_overwrite(dst_dfd, dst_rel, dst.mode, mode, out_ren);   /* temp file + rename for a regular target */
}

/* CONFLICT_ACT_* for moving src onto dst: overwrite when nothing is in the way (missing target, the same inode, two directories
 * on different devices: merged by the copy); otherwise the user decides */
intptr_t fo_move_check_conflict(const char *src, const char *dst, char *out_ren)
{
    FileStat s, d;
    if (os_statat(AT_FDCWD, dst, &d, 0) < 0 || os_statat(AT_FDCWD, src, &s, 0) < 0) return CONFLICT_ACT_OVERWRITE;
    if ((s.mode & S_IFMT) == S_IFDIR && (d.mode & S_IFMT) == S_IFDIR && s.dev != d.dev) return CONFLICT_ACT_OVERWRITE;
    if (s.dev == d.dev && s.ino == d.ino) return CONFLICT_ACT_OVERWRITE;
    return fs_task_resolve_conflict(AT_FDCWD, dst, (uint64_t)s.size, (uint64_t)d.size, out_ren);
}
