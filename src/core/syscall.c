/* src/core/syscall.c - blocking-I/O retries, raw terminal mode, signal flags, environment and subprocess helpers
 * (freestanding C23; replaces syscall.s). Kernel details stay behind the os_* PAL functions. */
#include "duop_string.h"
#include "duop_sys.h"

enum { EINTR_ = 4, EAGAIN_ = 11, AT_FDCWD_ = -100, O_RDWR_ = 2 };

volatile int32_t g_sigwinch;
volatile int32_t g_sigterm;
static volatile int32_t g_in_child;

intptr_t sys_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t total = 0;
    int stalls = 0;
    while (len) {
        intptr_t r = os_write(fd, p, len);
        if (r == -EINTR_) continue;
        if (r == -EAGAIN_) {
            if (stalls++ >= 20) return -EAGAIN_;
            intptr_t w;
            while ((w = os_poll_out(fd, 50)) == -EINTR_) {}
            if (w > 0) continue;
            return -EAGAIN_;
        }
        if (r < 0) return r;
        if (r == 0) break;
        stalls = 0;
        p += r;
        len -= (size_t)r;
        total += (size_t)r;
    }
    return (intptr_t)total;
}

intptr_t sys_read_retry(int fd, void *buf, size_t len)
{
    intptr_t r;
    while ((r = os_read(fd, buf, len)) == -EINTR_) {}
    return r;
}

int64_t sys_clock_ms(void) { return os_clock_ms(); }

intptr_t sys_fs_avail(int fd, uint64_t *avail)
{
    uint64_t sf[16];                                                      /* struct statfs (120 bytes) of the 64-bit Linux targets */
    intptr_t r = os_fstatfs(fd, sf);
    if (r < 0) return r;
    *avail = sf[1] * sf[4];                                               /* f_bsize * f_bavail */
    return 0;
}

void sys_get_winsize(int *cols, int *rows) { os_get_winsize(cols, rows); }

int term_enter_raw(void)
{
    static const char seq[] = "\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h";
    os_term_save_and_raw();
    (void)sys_write_all(1, seq, sizeof seq - 1);
    return 0;
}

void term_exit_raw(void)
{
    static const char seq[] = "\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?25h\x1b[0m\x1b[?1049l";
    (void)sys_write_all(1, seq, sizeof seq - 1);
    (void)os_term_restore();
}

static void on_sigwinch(void) { g_sigwinch = 1; }
static void on_sigint(void) { if (!g_in_child) g_sigterm = 1; }
static void on_sigterm(void) { g_sigterm = 1; }

void signals_install(void)
{
    os_signals_install(on_sigwinch, on_sigint, on_sigterm);
    os_fault_guard_install();                   /* unified TRY/CATCH + mmap SIGBUS guard */
}

char **g_envp;                          /* environment of the process: set once by the entry point, read by the git/preview helpers */

const char *env_getenv(char **envp, const char *name)
{
    if (!envp) return nullptr;
    size_t n = str_len(name);
    for (; *envp; envp++)
        if (str_starts_with(*envp, name) && (*envp)[n] == '=') return *envp + n + 1;
    return nullptr;
}

/* Waits for pid (retrying EINTR) and returns its raw wait status. */
static int wait_status(intptr_t pid)
{
    int status = 0;
    while (os_wait4((int)pid, &status, 0, nullptr) == -EINTR_) {}
    return status;
}

/* Runs a prepared command in a child and restores the terminal: shared by the foreground exec helpers. */
static int run_foreground(const char *exe, char *const *argv, char **envp)
{
    term_exit_raw();
    g_in_child = 1;
    intptr_t pid = os_fork();
    int rc;
    if (pid == 0) {
        os_sig_reset_pipe();
        (void)os_execve(exe, argv, envp);
        os_exit(127);
    }
    rc = pid < 0 ? (int)pid : wait_status(pid);
    g_in_child = 0;
    (void)term_enter_raw();
    return rc;
}

int sys_exec_sh_arg(const char *script, const char *arg1, char **envp)
{
    char *argv[6] = {(char *)"/bin/sh", (char *)"-c", (char *)script, (char *)"sh", (char *)arg1, nullptr};
    return run_foreground("/bin/sh", argv, envp);
}

int sys_exec_argv(const char *exe, char **argv, char **envp)
{
    return run_foreground(exe, argv, envp);
}

/* Background run with /dev/null stdio and without leaving raw mode (the caller still waits for the child). */
int sys_exec_argv_bg(const char *exe, char **argv, char **envp)
{
    intptr_t pid = os_fork();
    if (pid < 0) return (int)pid;
    if (pid == 0) {
        os_sig_reset_pipe();
        os_mark_child_forked();
        intptr_t fd = os_openat(AT_FDCWD_, "/dev/null", O_RDWR_, 0);
        if (fd >= 0) {
            for (int i = 0; i < 3; i++) (void)os_dup2((int)fd, i);
            if (fd > 2) (void)os_close((int)fd);
        }
        (void)os_execve(exe, argv, envp);
        os_exit(127);
    }
    return wait_status(pid);
}

/* xdg-open through a double fork so the opener is re-parented and never becomes our zombie. */
int sys_open_external(const char *path, char **envp)
{
    if (!env_getenv(envp, "DISPLAY") && !env_getenv(envp, "WAYLAND_DISPLAY")) return -1;
    intptr_t pid = os_fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (os_fork() != 0) os_exit(0);
        os_sig_reset_pipe();
        os_mark_child_forked();
        char *argv[6] = {(char *)"/bin/sh", (char *)"-c", (char *)"xdg-open \"$1\" >/dev/null 2>&1 </dev/null",
                         (char *)"sh", (char *)path, nullptr};
        (void)os_execve("/bin/sh", argv, envp);
        os_exit(127);
    }
    (void)wait_status(pid);
    return 0;
}
