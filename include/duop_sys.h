/* include/duop_sys.h - core process/terminal helpers (src/core/syscall.c, try_rt.c). Freestanding C23. */
#pragma once
#include "duop_abi.h"

#define STDIN_FD 0
#define STDOUT_FD 1
#define STDERR_FD 2

extern volatile int32_t g_sigwinch;     /* set by the SIGWINCH handler, cleared by the event loop */
extern volatile int32_t g_sigterm;      /* set by SIGTERM / SIGINT (outside child runs) */
extern char **g_envp;                  /* process environment (src/core/syscall.c); the entry point stores envp here at startup */
extern void *g_try_frame;               /* head of the active TRY frame chain (asm macros in try_using.inc, os_try_begin for C) */
static inline void os_try_end(TryFrame *frame) { g_try_frame = frame->prev; }   /* leave the TRY normally: unlink the frame */

DUOP_NODISCARD intptr_t    sys_write_all(int fd, const void *buf, size_t len);       /* total written or -errno; retries EINTR/EAGAIN */
DUOP_NODISCARD intptr_t    sys_read_retry(int fd, void *buf, size_t len);            /* read(2) retried on EINTR */
int64_t     sys_clock_ms(void);
DUOP_NODISCARD intptr_t sys_fs_avail(int fd, uint64_t *avail);   /* bytes available to an unprivileged writer on the file system of fd: 0 or -errno */
void        sys_get_winsize(int *cols, int *rows);
int         term_enter_raw(void);
void        term_exit_raw(void);
void        signals_install(void);
const char *env_getenv(char **envp, const char *name);                /* value after '=' or NULL */
int         sys_exec_sh_arg(const char *script, const char *arg1, char **envp);   /* wait status, or -errno */
int         sys_exec_argv(const char *exe, char **argv, char **envp);
int         sys_exec_argv_bg(const char *exe, char **argv, char **envp);
int         sys_open_external(const char *path, char **envp);         /* 0 launched, -1 without a GUI display */
void        os_fault_guard_install(void);

int         tmp_ensure_dir(char *out_dir, int cap);                     /* the private temp directory: 0 / -1 */
DUOP_NODISCARD int         tmp_make_path(const char *prefix, const char *suffix, char *out_path, int cap);   /* created and closed: 0 / -1 */
DUOP_NODISCARD intptr_t    tmp_open_unique(const char *prefix, const char *suffix, char *out_path, int cap);   /* $TMPDIR/duop-<uid>/<prefix>XXXX<suffix>: fd or -errno */
/* crash-safe writes (src/core/tmpfile.c): open <final>.tmp.<hex> exclusively, then fsync + rename it over final */
DUOP_NODISCARD intptr_t    tmp_open_atomic(const char *final_path, char *tmp_buf, int mode);   /* tmp_buf: PATH_MAX bytes; fd or -errno */
DUOP_NODISCARD intptr_t    tmp_commit_atomic(int fd, const char *tmp_buf, const char *final_path, int64_t write_rc);   /* write_rc < 0 aborts; 0 or -errno */
