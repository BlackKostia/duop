/* include/duop_pal.h - kernel ABI constants for portable C, mirrored from include/os/<os>/<arch>/pal_target.inc.
 * tests/test_abi_cross.py proves every value here equals the .inc (and so the target's UAPI headers). Only values
 * that portable code needs; kernel structs never appear here (see FileStat in duop_abi.h). */
#pragma once

/* errno values returned as -errno by every os_* function (generic Linux numbering) */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EFBIG 27
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define EBADMSG 74
#define EOPNOTSUPP 95
#define ECANCELED 125

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define RENAME_NOREPLACE 1

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR 0x2
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_CLOEXEC 0x80000
#if defined(__x86_64__)
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW 0x20000
#else                                       /* arm64 / arm generic values */
#define O_DIRECTORY 0x4000
#define O_NOFOLLOW 0x8000
#endif

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_RW 0x3
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE_ANON 0x22
#define MAP_SHARED_ANON 0x21
#define MAP_FIXED 0x10
#define MAP_POPULATE 0x8000
#define MREMAP_MAYMOVE 0x1

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFIFO 0010000

/* inotify event bits (pal_target.inc PAL_IN_*; identical on every Linux target) */
#define PAL_IN_MODIFY 0x00000002
#define PAL_IN_ATTRIB 0x00000004
#define PAL_IN_MOVED_FROM 0x00000040
#define PAL_IN_MOVED_TO 0x00000080
#define PAL_IN_CREATE 0x00000100
#define PAL_IN_DELETE 0x00000200
#define PAL_IN_DELETE_SELF 0x00000400
#define PAL_IN_MOVE_SELF 0x00000800
#define PAL_IN_Q_OVERFLOW 0x00004000

#define SIGKILL 9                             /* process control: pkg_fetch_url kills a timed-out helper */
#define SIGTERM 15
#define SIGCONT 18                            /* pause / resume of the background worker */
#define SIGSTOP 19
#define WNOHANG 1
#define PR_SET_NO_NEW_PRIVS 38                /* prctl: a previewer child gains no privileges by exec */

#define OS_AT_EXECFN 31                       /* auxv: path given to execve */

#define PAL_DT_UNKNOWN 0                      /* d_type of a directory entry (generic Linux values) */
#define PAL_DT_FIFO 1
#define PAL_DT_CHR 2
#define PAL_DT_DIR 4
#define PAL_DT_BLK 6
#define PAL_DT_REG 8
#define PAL_DT_LNK 10
#define PAL_DT_SOCK 12
