# src/os/linux/x86_64/pal_sys_path.s - C-callable raw syscall wrappers: path (*at) operations
# SysV args -> syscall regs (rcx -> r10); returns -errno in rax (no errno global, no runtime OS checks).
.intel_syntax noprefix
.include "pal.inc"

.macro PAL_SYS name, nr
.section .text.\name, "ax", @progbits
.global \name
\name:
    mov r10, rcx
    mov eax, \nr
    syscall
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
