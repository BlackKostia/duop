// src/os/linux/aarch64/pal_sys_path.s - C-callable raw syscall wrappers: path (*at) operations
// AAPCS64 args x0-x5 are already the syscall registers; returns -errno in x0.
.include "pal.inc"

.macro PAL_SYS name, nr
.section .text.\name, "ax", @progbits
.global \name
\name:
    mov x8, #\nr
    svc #0
    ret
.endm

PAL_SYS os_mkdirat, OS_SYS_mkdirat
PAL_SYS os_unlinkat, OS_SYS_unlinkat
PAL_SYS os_renameat2, OS_SYS_renameat2
PAL_SYS os_symlinkat, OS_SYS_symlinkat
PAL_SYS os_readlinkat, OS_SYS_readlinkat
PAL_SYS os_linkat, OS_SYS_linkat
PAL_SYS os_fchmodat, OS_SYS_fchmodat
PAL_SYS os_fchownat, OS_SYS_fchownat
PAL_SYS os_utimensat, OS_SYS_utimensat
PAL_SYS os_fstatfs, OS_SYS_fstatfs
PAL_SYS os_getcwd, OS_SYS_getcwd
PAL_SYS os_chdir, OS_SYS_chdir
