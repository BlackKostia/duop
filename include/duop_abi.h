/* include/duop_abi.h - freestanding C23 view of the Duop PAL ABI (Етап 0.1).
 *
 * Only compiler headers (<stdint.h>, <stddef.h>, <stdbool.h>) -- never libc. Kernel structs stay in
 * src/os/; portable code sees FileStat and the os_* functions below. Every os_* that can fail returns
 * a non-negative result or -errno (see os_is_err); there is no errno global.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DUOP_NORETURN    [[noreturn]]
#define DUOP_NODISCARD   [[nodiscard]]
#define DUOP_UNUSED      [[maybe_unused]]
#define DUOP_RETURNS_TWICE __attribute__((returns_twice))
#define DUOP_RODATA      __attribute__((section(".rodata")))
#define DUOP_PACKED      __attribute__((packed))

/* Fixed-width mirror of the `.struct FileStat` in include/pal.inc (56 bytes on every target). */
typedef struct FileStat {
    uint64_t dev, ino;
    int64_t  size, mtime;
    uint32_t mode, mtime_nsec;
    uint64_t nlink;
    uint32_t uid, gid;
} FileStat;
_Static_assert(sizeof(FileStat) == 56, "FileStat must match include/pal.inc");
_Static_assert(offsetof(FileStat, mode) == 32 && offsetof(FileStat, uid) == 48, "FileStat layout");

/* Polite busy-wait step for spinlocks (x86 PAUSE, ARM YIELD). */
static inline void duop_cpu_relax(void)
{
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Error contract: results in [-4095, -1] are -errno. */
static inline bool os_is_err(intptr_t r) { return (uintptr_t)r > (uintptr_t)-4096; }

/* Unaligned little-endian loads for parsers (ARMv7 faults on unaligned LDM/LDRD; C aliasing is UB). */
static inline uint16_t load_le16(const void *p) { uint16_t v; __builtin_memcpy(&v, p, 2); return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? v : __builtin_bswap16(v); }
static inline uint32_t load_le32(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? v : __builtin_bswap32(v); }
static inline uint64_t load_le64(const void *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? v : __builtin_bswap64(v); }

/* ---- Raw syscall wrappers (src/os/<os>/<arch>/pal_sys_*.s) ---- */
DUOP_NODISCARD intptr_t os_read(int fd, void *buf, size_t n);
DUOP_NODISCARD intptr_t os_write(int fd, const void *buf, size_t n);
intptr_t os_close(int fd);
DUOP_NODISCARD intptr_t os_fsync(int fd);
DUOP_NODISCARD intptr_t os_ftruncate(int fd, int64_t len);
intptr_t os_lseek(int fd, int64_t off, int whence);
intptr_t os_ioctl(int fd, uintptr_t req, void *arg);
intptr_t os_fcntl(int fd, int cmd, uintptr_t arg);
DUOP_NODISCARD intptr_t os_pipe2(int fds[2], int flags);
DUOP_NODISCARD intptr_t os_openat(int dirfd, const char *path, int flags, unsigned mode);
intptr_t os_dup2(int oldfd, int newfd);
DUOP_NODISCARD intptr_t os_mkdirat(int dirfd, const char *path, unsigned mode);
intptr_t os_unlinkat(int dirfd, const char *path, int flags);
DUOP_NODISCARD intptr_t os_renameat2(int olddir, const char *old, int newdir, const char *new_, unsigned flags);
DUOP_NODISCARD intptr_t os_symlinkat(const char *target, int newdirfd, const char *linkpath);
DUOP_NODISCARD intptr_t os_readlinkat(int dirfd, const char *path, char *buf, size_t n);
DUOP_NODISCARD intptr_t os_linkat(int olddir, const char *old, int newdir, const char *new_, int flags);
DUOP_NODISCARD intptr_t os_fchmodat(int dirfd, const char *path, unsigned mode);
DUOP_NODISCARD intptr_t os_utimensat(int dirfd, const char *path, const int64_t ts[4], int flags);   /* ts = {atime s, ns, mtime s, ns}, NULL = now; path NULL: the file of dirfd (futimens) */
DUOP_NODISCARD intptr_t os_fstatfs(int fd, void *statfs_buf);   /* struct statfs: 120 bytes, f_bsize at 8, f_bavail at 32 on x86_64 and aarch64 */
DUOP_NODISCARD intptr_t os_fchownat(int dirfd, const char *path, unsigned uid, unsigned gid, int flags);
intptr_t os_getcwd(char *buf, size_t n);
intptr_t os_chdir(const char *path);
DUOP_NODISCARD intptr_t os_fork(void);
intptr_t os_execve(const char *path, char *const argv[], char *const envp[]);
intptr_t os_wait4(int pid, int *status, int options, void *rusage);
intptr_t os_kill(int pid, int sig);
DUOP_NORETURN void os_exit(int code);
intptr_t os_getpid(void);
intptr_t os_getppid(void);
intptr_t os_gettid(void);
intptr_t os_setsid(void);
intptr_t os_getuid(void);
intptr_t os_prctl(int op, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
intptr_t os_prlimit64(int pid, int resource, const void *new_limit, void *old_limit);
intptr_t os_tgkill(int tgid, int tid, int sig);
intptr_t os_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
intptr_t os_munmap_raw(void *addr, size_t len);                       /* plain munmap, no SIGBUS-registry update */
intptr_t os_sigaltstack(const void *ss, void *old_ss);
intptr_t os_rt_sigaction(int sig, const void *act, void *old, size_t sigsetsize);
intptr_t os_rt_sigprocmask(int how, const void *set, void *old, size_t sigsetsize);

/* Memory-check hooks (src/core/memcheck.c, MEMCHECK builds only; main.s calls mc_init / mc_report) */
void mc_on_alloc(void *p, size_t size, void *caller);
void mc_on_free(void *p, void *caller);
void mc_on_realloc(void *old_p, void *new_p, size_t size, void *caller);
void mc_init(char **envp);                    /* $DUOP_MEMCHECK_LOG selects the log file, default stderr */
void mc_report(void);                         /* MC_SUMMARY + one MC_LEAK line per live block */

/* ---- Normalised PAL (src/os/<os>/<arch>/pal_os.s and siblings) ---- */
DUOP_NODISCARD int       os_statat(int dirfd, const char *path, FileStat *out, int follow_symlinks);
DUOP_NODISCARD int       os_fstat(int fd, FileStat *out);
intptr_t  os_dir_read(int dirfd, void *buf, size_t cap);
const char *os_dir_next(void *buf, size_t bytes, size_t *pos);
const char *os_dir_next_ex(void *buf, size_t bytes, size_t *pos, uint8_t *out_type);
void     *os_mem_remap(void *old_base, size_t old_mapped, size_t new_mapped);
int       os_poll_in(int fd, int timeout_ms);
int       os_poll_in2(int fd0, int fd1, int timeout_ms);
int       os_poll_many(const int *fds, int n, int timeout_ms);
int       os_set_nonblock(int fd);
int       os_pipe_cloexec(int fds[2]);
int       os_clone_file(int src_fd, int dst_fd);
intptr_t  os_copy_file_range(int src_fd, int dst_fd, size_t len);
void     *os_mmap_file_ro(int fd, size_t len);
void      os_munmap(void *addr, size_t len);
void      os_sigbus_guard_install(void);
int64_t   os_clock_ms(void);
int64_t   os_time_sec(void);                                  /* CLOCK_REALTIME seconds; 0 when the clock cannot be read */
int       os_watch_init(void);                                /* non-blocking close-on-exec inotify fd, or -errno */
int       os_watch_add(int fd, const char *path, uint32_t mask);   /* watch descriptor, or -errno */
int       os_watch_rm(int fd, int wd);
intptr_t  os_watch_read(int fd, void *buf, size_t cap);       /* bytes of queued events, or -errno (-EAGAIN: none) */
int       os_watch_next(const void *buf, size_t len, size_t *pos, int *wd, uint32_t *mask);   /* 1 and the next event, 0 at the end */
int       os_thread_spawn(void (*fn)(void *), void *arg, void *stack_top, int *ctid_out);   /* >= 0, or -errno; *ctid_out is cleared by the kernel at thread exit */
void      os_thread_join(int *ctid);                          /* futex-waits until *ctid becomes 0 */
int64_t   os_du_usage(int dirfd, const char *name, FileStat *out);   /* allocated bytes (lstat semantics) or -errno */
int       os_fstat_du(int fd, FileStat *out, int64_t *alloc_out);    /* 0 or -errno; alloc_out = the directory's own allocated bytes */
int       os_fs_is_derived(int fd);                           /* 1: proc / sysfs / FUSE / squashfs / overlay / network (or unidentifiable) */
int       os_batch_stat(int dirfd, const char *const *names, FileStat *out_stats, int32_t *out_errs, size_t count, int follow_symlinks);
void      os_get_winsize(int *cols, int *rows);
void      os_get_winsize_px(int *xpix, int *ypix);   /* terminal size in pixels (0, 0 when not reported) */
void      os_term_save_and_raw(void);
int       os_term_restore(void);
void      os_signals_worker_mode(void);
int       os_watch_init(void);
int       os_watch_add(int fd, const char *path, uint32_t pal_in_mask);
int       os_watch_rm(int fd, int wd);
intptr_t  os_watch_read(int fd, void *buf, size_t cap);
int       os_watch_next(const void *buf, size_t len, size_t *pos, int *wd, uint32_t *mask);

int       os_poll_out(int fd, int timeout_ms);     /* >0 writable, 0 timeout, -errno */
void      os_sig_reset_pipe(void);                  /* SIG_DFL for SIGPIPE in a forked child */
void      os_mark_child_forked(void);               /* reset the child's TRY owner and inherited slot */
int       os_set_pdeathsig(int expected_ppid, int sig);   /* 1 parent alive, 0 it died in the race (the child then exits) */
int       os_parent_alive(int expected_ppid);       /* 1 while getppid() == expected_ppid */
void      os_raise_nofile(int want);                /* RLIMIT_NOFILE soft limit raised towards want */
void      os_uring_atfork_reset(void);              /* drop the inherited io_uring mappings / fd in a forked child */
void      os_signals_install(void (*on_winch)(void), void (*on_int)(void), void (*on_term)(void));
void      os_fault_guard_install_slot(void **try_frame_slot);
int       os_pty_spawn(int *out_master_fd, int *out_pid, const char *cwd, char **envp, int cols, int rows);
void      os_pty_resize(int master_fd, int cols, int rows);
DUOP_NODISCARD intptr_t os_pty_read(int fd, void *buf, size_t len);
DUOP_NODISCARD intptr_t os_pty_write(int fd, const void *buf, size_t len);
void      os_pty_close(int fd);

/* ---- TRY/CATCH fault guard for C (pal_fault.s). A TryFrame is setjmp-like: os_try_begin returns 0, and when a SIGSEGV / SIGBUS / SIGFPE
 * hits this thread before os_try_end the handler unlinks the frame, restores the callee-saved registers and sp and makes os_try_begin
 * return a second time with the signal number. Locals changed after os_try_begin and read after the fault must be volatile. The x86_64
 * layout is the assembly TRY_BEGIN frame of include/try_using.inc, so asm and C frames share one chain (g_try_frame). */
#if defined(__x86_64__)
#define TRY_FRAME_REGS 8                       /* rbx rbp r12 r13 r14 r15 rsp rip */
#elif defined(__aarch64__)
#define TRY_FRAME_REGS 14                      /* x19..x30 sp pc */
#else
#define TRY_FRAME_REGS 16                      /* declaration only: no PAL implementation yet */
#endif
typedef struct TryFrame { void *prev; uintptr_t regs[TRY_FRAME_REGS]; } TryFrame;
_Static_assert(sizeof(TryFrame) == (TRY_FRAME_REGS + 1) * sizeof(void *), "TryFrame is shared with pal_fault.s");
int       os_try_begin(TryFrame *frame) __attribute__((returns_twice));

/* ---- SIMD/scalar primitives (pal_simd.s) ---- */
void   os_mem_copy_fast(void *dst, const void *src, size_t n);   /* non-overlapping */
void   os_mem_move_fast(void *dst, const void *src, size_t n);   /* overlap-safe */
void   os_mem_set_fast(void *dst, int byte, size_t n);
int    os_mem_cmp_fast(const void *a, const void *b, size_t n);  /* 0 when equal, else sign of first difference */
size_t os_str_len_fast(const char *s);
int    os_str_contains_ci_fast(const char *hay, const char *needle_lc, size_t nlen);
size_t os_mem_mismatch16(const void *a, const void *b, size_t n16);
size_t os_mem_find_byte(const void *p, size_t len, int byte);
size_t os_mem_count_byte(const void *p, size_t len, int byte);

/* ---- Entry (pal_start.s -> application) ---- */
int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv);
