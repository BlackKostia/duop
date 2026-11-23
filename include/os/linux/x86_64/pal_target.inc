# Duop Compile-Time PAL Target Definitions: Linux x86-64
# Verified against Linux 7.0 / Ubuntu 24.04 x86-64 kernel ABI headers.
# Kernel syscall numbers and structs stay encapsulated here and in src/os/linux/x86_64/.
.intel_syntax noprefix
.ifndef PAL_TARGET_INCLUDED
.equ PAL_TARGET_INCLUDED, 1

# --- Linux x86-64 Kernel Syscall Numbers (private to OS PAL) ---
.equ OS_SYS_read,           0
.equ OS_SYS_write,          1
.equ OS_SYS_close,          3
.equ OS_SYS_fstat,          5
.equ OS_SYS_mmap,           9
.equ OS_SYS_munmap,         11
.equ OS_SYS_rt_sigaction,   13
.equ OS_SYS_rt_sigprocmask, 14
.equ OS_SYS_rt_sigreturn,   15
.equ OS_SYS_ioctl,          16
.equ OS_SYS_mremap,         25
.equ OS_SYS_dup2,           33
.equ OS_SYS_getpid,         39
.equ OS_SYS_clone,          56
.equ OS_SYS_fork,           57
.equ OS_SYS_futex,          202
.equ OS_SYS_execve,         59
.equ OS_SYS_exit,           60
.equ OS_SYS_wait4,          61
.equ OS_SYS_kill,           62
.equ OS_SYS_fcntl,          72
.equ OS_SYS_fsync,          74
.equ OS_SYS_getcwd,         79
.equ OS_SYS_chdir,          80
.equ OS_SYS_getuid,         102
.equ OS_SYS_getdents64,     217
.equ OS_SYS_inotify_add_watch, 254
.equ OS_SYS_inotify_rm_watch, 255
.equ OS_SYS_clock_gettime,  228
.equ OS_SYS_openat,         257
.equ OS_SYS_mkdirat,        258
.equ OS_SYS_newfstatat,     262
.equ OS_SYS_unlinkat,       263
.equ OS_SYS_renameat,       264
.equ OS_SYS_renameat2,      316
.equ RENAME_NOREPLACE,      1
.equ OS_SYS_linkat,         265
.equ OS_SYS_symlinkat,      266
.equ OS_SYS_readlinkat,     267
.equ OS_SYS_fchmodat,       268
.equ OS_SYS_fchownat,       260
.equ OS_SYS_ppoll,          271
.equ OS_SYS_pipe2,          293
.equ OS_SYS_inotify_init1,  294
.equ OS_SYS_copy_file_range, 326
.equ OS_SYS_statx,          332
.equ OS_SYS_io_uring_setup, 425
.equ OS_SYS_io_uring_enter, 426

# --- Portable POSIX File Descriptors & *at Flags ---
.equ STDIN_FD,              0
.equ STDOUT_FD,             1
.equ STDERR_FD,             2
.equ AT_FDCWD,              -100
.equ AT_SYMLINK_NOFOLLOW,   0x100
.equ AT_REMOVEDIR,          0x200
.equ OS_AT_EXECFN,          31      # auxv: path given to execve

# --- Portable Open Flags (Linux x86-64 values) ---
.equ O_RDONLY,              0x0
.equ O_WRONLY,              0x1
.equ O_RDWR,                0x2
.equ O_CREAT,               0x40
.equ O_EXCL,                0x80
.equ O_TRUNC,               0x200
.equ O_APPEND,              0x400
.equ O_NONBLOCK,            0x800
.equ O_DIRECTORY,           0x10000
.equ O_NOFOLLOW,            0x20000
.equ O_CLOEXEC,             0x80000

# --- Memory Mapping Flags ---
.equ PROT_READ,             0x1
.equ PROT_WRITE,            0x2
.equ PROT_RW,               0x3
.equ MAP_SHARED,            0x01
.equ MAP_PRIVATE,           0x02
.equ MAP_ANONYMOUS,         0x20
.equ MAP_PRIVATE_ANON,      0x22
.equ MAP_SHARED_ANON,       0x21
.equ MAP_FIXED,             0x10
.equ MAP_POPULATE,          0x8000
.equ MREMAP_MAYMOVE,        0x1

# --- POSIX File Mode Types ---
.equ S_IFMT,                0170000
.equ S_IFDIR,               0040000
.equ S_IFREG,               0100000
.equ S_IFLNK,               0120000

# --- Common Errno Values (PAL normalizes failures to -errno) ---
.equ EPERM,                 1
.equ ENOENT,                2
.equ ESRCH,                 3
.equ EINTR,                 4
.equ EIO,                   5
.equ EBADF,                 9
.equ ECHILD,                10
.equ EAGAIN,                11
.equ ENOMEM,                12
.equ EACCES,                13
.equ EBUSY,                 16
.equ EEXIST,                17
.equ EXDEV,                 18
.equ ENOTDIR,               20
.equ EISDIR,                21
.equ EINVAL,                22
.equ ENAMETOOLONG,          36
.equ EFBIG,                 27
.equ EBADMSG,               74
.equ ENOSYS,                38
.equ ENOTEMPTY,             39
.equ EOPNOTSUPP,            95
.equ ECANCELED,             125

# --- Portable directory-watch event bits (Linux inotify values) ---
.equ PAL_IN_MODIFY,         0x00000002
.equ PAL_IN_ATTRIB,         0x00000004
.equ PAL_IN_MOVED_FROM,     0x00000040
.equ PAL_IN_MOVED_TO,       0x00000080
.equ PAL_IN_CREATE,         0x00000100
.equ PAL_IN_DELETE,         0x00000200
.equ PAL_IN_DELETE_SELF,    0x00000400
.equ PAL_IN_MOVE_SELF,      0x00000800
.equ PAL_IN_Q_OVERFLOW,     0x00004000
.equ KINOTIFY_HDR,          16       # struct inotify_event: wd, mask, cookie, len, name[]
.equ F_GETFL,               3
.equ F_SETFL,               4

# --- Portable Process Control (signals & wait flags) ---
.equ SIGKILL,               9
.equ SIGCONT,               18
.equ SIGSTOP,               19
.equ WNOHANG,               1
.equ PR_SET_NO_NEW_PRIVS,   38

# --- Linux Kernel ABI Struct Offsets (only used inside src/os/linux/x86_64/) ---
.equ TCGETS,                0x5401
.equ TCSETS,                0x5402
.equ TCSETSW,               0x5403
.equ TIOCGWINSZ,            0x5413
.equ FICLONE,               0x40049409

.equ KTERMIOS_SIZE,         64
.equ KTERMIOS_IFLAG,        0
.equ KTERMIOS_OFLAG,        4
.equ KTERMIOS_CFLAG,        8
.equ KTERMIOS_LFLAG,        12
.equ KTERMIOS_VTIME,        22
.equ KTERMIOS_VMIN,         23

.equ ICANON,                0x0002
.equ ECHO,                  0x0008
.equ ISIG,                  0x0001
.equ IEXTEN,                0x8000
.equ IXON,                  0x0400
.equ ICRNL,                 0x0100
.equ BRKINT,                0x0002
.equ INPCK,                 0x0010
.equ ISTRIP,                0x0020
.equ OPOST,                 0x0001
.equ CS8,                   0x0030

.equ WINSIZE_ROW,           0
.equ WINSIZE_COL,           2
.equ KSIGACTION_SIZE,       32
.equ KSIG_HANDLER,          0
.equ KSIG_FLAGS,            8
.equ KSIG_RESTORER,         16
.equ KSIG_MASK,             24
.equ SA_RESTORER,           0x04000000
.equ KSIG_IGN,              1
.equ SA_SIGINFO,            0x00000004
.equ SIGABRT,               6
.equ SIGBUS,                7
.equ SIGFPE,                8
.equ SIGSEGV,               11
.equ SIGPIPE,               13
.equ KSIGINFO_ADDR,         16
.equ KUC_RET,               144          # ucontext_t.uc_mcontext.gregs[REG_RAX]: the value a handler's resume sees in rax
.equ KUC_SP,                160          # gregs[REG_RSP]
.equ KUC_PC,                168          # gregs[REG_RIP]
.equ SIGINT,                2
.equ SIGTERM,               15
.equ SIGWINCH,              28
.equ POLLFD_FD,             0
.equ POLLFD_EVENTS,         4
.equ POLLFD_REVENTS,        6
.equ POLLIN,                0x0001; .equ POLLOUT, 0x0004
.equ CLOCK_MONOTONIC,       1
.equ TIMESPEC_SEC,          0
.equ TIMESPEC_NSEC,         8

.equ KSTAT_SIZE,            144
.equ KSTAT_DEV,             0
.equ KSTAT_INO,             8
.equ KSTAT_NLINK,           16
.equ KSTAT_MODE,            24
.equ KSTAT_UID,             28
.equ KSTAT_GID,             32
.equ KSTAT_ST_SIZE,         48
.equ KSTAT_MTIME,           88
.equ KSTAT_MTIME_NSEC,      96

.equ KDIRENT_INO,           0
.equ KDIRENT_RECLEN,        16
.equ KDIRENT_TYPE,          18
.equ KDIRENT_NAME,          19

# --- Linux statx & io_uring Kernel ABI Constants (private to OS PAL) ---
.equ STATX_BASIC_STATS,     0x7ff
.equ KSTATX_SIZE,           256
.equ KSTATX_MASK,           0
.equ KSTATX_MODE,           28
.equ KSTATX_INO,            32
.equ KSTATX_ST_SIZE,        40
.equ KSTATX_MTIME,          112
.equ KSTATX_DEV_MAJOR,      136
.equ KSTATX_DEV_MINOR,      140

.equ IORING_OP_STATX,       21
.equ IORING_ENTER_GETEVENTS, 1
.equ IORING_FEAT_SINGLE_MMAP, 1
.equ IORING_OFF_SQ_RING,    0
.equ IORING_OFF_CQ_RING,    0x8000000
.equ IORING_OFF_SQES,       0x10000000

.equ KSQE_SIZE,             64
.equ KSQE_OP,               0
.equ KSQE_FLAGS,            1
.equ KSQE_FD,               4
.equ KSQE_OFF,              8
.equ KSQE_ADDR,             16
.equ KSQE_LEN,              24
.equ KSQE_STATX_FLAGS,      28
.equ KSQE_UDATA,            32

.equ KCQE_SIZE,             16
.equ KCQE_UDATA,            0
.equ KCQE_RES,              8
.equ KCQE_FLAGS,            12

.equ KIOU_PARAMS_SIZE,      120
.equ KIOU_P_SQ_ENT,         0
.equ KIOU_P_CQ_ENT,         4
.equ KIOU_P_FEAT,           20
.equ KIOU_P_SQ_OFF,         40
.equ KIOU_P_CQ_OFF,         80
.equ KIOU_SQO_HEAD,         0
.equ KIOU_SQO_TAIL,         4
.equ KIOU_SQO_MASK,         8
.equ KIOU_SQO_ENT,          12
.equ KIOU_SQO_ARRAY,        24
.equ KIOU_CQO_HEAD,         0
.equ KIOU_CQO_TAIL,         4
.equ KIOU_CQO_MASK,         8
.equ KIOU_CQO_ENT,          12
.equ KIOU_CQO_CQES,         20

# ==============================================================================
# Zero-Overhead Inline Syscall PAL Macros (Linux x86-64)
# Convention: register arguments follow Linux/POSIX syscall ABI (rdi, rsi, rdx, r10);
# return value in rax >= 0 on success or -errno < 0 on failure.
# ==============================================================================

.macro OS_READ
    mov eax, OS_SYS_read
    syscall
.endm

.macro OS_WRITE
    mov eax, OS_SYS_write
    syscall
.endm

.macro OS_OPENAT
    mov eax, OS_SYS_openat
    syscall
.endm

.macro OS_CLOSE; mov eax, OS_SYS_close; syscall; .endm
.macro OS_FSYNC; mov eax, OS_SYS_fsync; syscall; .endm

.macro OS_MKDIRAT
    mov eax, OS_SYS_mkdirat
    syscall
.endm

.macro OS_UNLINKAT
    mov eax, OS_SYS_unlinkat
    syscall
.endm

.macro OS_RENAMEAT; mov eax, OS_SYS_renameat; syscall; .endm
.macro OS_RENAMEAT2; mov eax, OS_SYS_renameat2; syscall; .endm

.macro OS_SYMLINKAT
    mov eax, OS_SYS_symlinkat
    syscall
.endm

.macro OS_READLINKAT
    mov eax, OS_SYS_readlinkat
    syscall
.endm

.macro OS_LINKAT
    mov eax, OS_SYS_linkat
    syscall
.endm

.macro OS_FCHMODAT
    mov eax, OS_SYS_fchmodat
    syscall
.endm

.macro OS_FCHOWNAT
    mov eax, OS_SYS_fchownat
    syscall
.endm

.macro OS_GETCWD
    mov eax, OS_SYS_getcwd
    syscall
.endm

.macro OS_CHDIR
    mov eax, OS_SYS_chdir
    syscall
.endm

# Allocate anonymous private pages: rdi=addr(0), rsi=length, rdx=prot
.macro OS_MMAP_ANON
    mov r10d, MAP_PRIVATE_ANON
    mov r8, -1
    xor r9d, r9d
    mov eax, OS_SYS_mmap
    syscall
.endm

.macro OS_MUNMAP
    mov eax, OS_SYS_munmap
    syscall
.endm

.macro OS_FORK
    mov eax, OS_SYS_fork
    syscall
.endm

.macro OS_EXECVE
    mov eax, OS_SYS_execve
    syscall
.endm

.macro OS_WAIT4
    mov eax, OS_SYS_wait4
    syscall
.endm

.macro OS_PIPE2
    mov eax, OS_SYS_pipe2
    syscall
.endm

.macro OS_DUP2
    mov eax, OS_SYS_dup2
    syscall
.endm

# Send signal: rdi=pid, rsi=sig
.macro OS_KILL
    mov eax, OS_SYS_kill
    syscall
.endm
# Allocate anonymous pages shared with forked children: rdi=addr(0), rsi=length, rdx=prot
.macro OS_MMAP_SHARED_ANON
    mov r10d, MAP_SHARED_ANON
    mov r8, -1
    xor r9d, r9d
    mov eax, OS_SYS_mmap
    syscall
.endm
.macro OS_SETSID; mov eax, OS_SYS_setsid; syscall; .endm
.macro OS_GETPID; mov eax, OS_SYS_getpid; syscall; .endm
.macro OS_GETUID; mov eax, OS_SYS_getuid; syscall; .endm
.macro OS_EXIT; mov eax, OS_SYS_exit; syscall; .endm
.macro OS_NO_NEW_PRIVS; mov edi, 38; mov esi, 1; xor edx, edx; xor r10d, r10d; xor r8d, r8d; mov eax, 157; syscall; .endm
.include "os/linux/x86_64/pal_abi_ext.inc"
.endif
