/* Freestanding harness for os_utimensat / os_fstatfs / sys_fs_avail. Script on stdin:
 *   T <path> <asec> <ansec> <msec> <mnsec> <flags>   os_utimensat(AT_FDCWD, path, {a, m}, flags)      -> "T <rc>"
 *   N <path> <flags>                                 os_utimensat(AT_FDCWD, path, NULL, flags) (both times = now) -> "N <rc>"
 *   Q <path> <asec> <ansec> <msec> <mnsec>           open the file, os_utimensat(fd, NULL, ts, 0) (futimens) -> "Q <rc>"
 *   O <path>                                         open the file, sys_fs_avail: "O <rc> <bytes available to unprivileged users>"
 *   B                                                sys_fs_avail(-1): "B <rc>" */
#include "script_cli.h"
#include "duop_pal.h"

static int64_t dec_signed(const char **p)
{
    while (**p == ' ') (*p)++;
    bool neg = **p == '-';
    if (neg) (*p)++;
    uint64_t v;
    *p = sc_dec(*p, &v);
    return neg ? -(int64_t)v : (int64_t)v;
}

static const char *word(const char *p, char *out, size_t cap)
{
    while (*p == ' ') p++;
    size_t n = 0;
    while (*p && *p != ' ' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return p;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    char *script = sc_read_all(), path[4096];
    for (char *line; (line = sc_next_line(&script));) {
        char cmd = line[0];
        const char *p = line + 1;
        if (cmd == 'T') {
            p = word(p, path, sizeof path);
            int64_t ts[4];
            for (int i = 0; i < 4; i++) ts[i] = dec_signed(&p);
            int flags = (int)dec_signed(&p);
            sc_put("T "); sc_num(os_utimensat(AT_FDCWD, path, ts, flags));
        } else if (cmd == 'N') {
            p = word(p, path, sizeof path);
            int flags = (int)dec_signed(&p);
            sc_put("N "); sc_num(os_utimensat(AT_FDCWD, path, nullptr, flags));
        } else if (cmd == 'Q') {
            p = word(p, path, sizeof path);
            int64_t ts[4];
            for (int i = 0; i < 4; i++) ts[i] = dec_signed(&p);
            intptr_t fd = os_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
            sc_put("Q ");
            if (os_is_err(fd)) sc_num(fd);
            else { sc_num(os_utimensat((int)fd, nullptr, ts, 0)); (void)os_close((int)fd); }
        } else if (cmd == 'O') {
            (void)word(p, path, sizeof path);
            intptr_t fd = os_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
            uint64_t avail = 0;
            sc_put("O ");
            if (os_is_err(fd)) { sc_num(fd); }
            else { sc_num(sys_fs_avail((int)fd, &avail)); sc_put(" "); sc_num((int64_t)avail); (void)os_close((int)fd); }
        } else if (cmd == 'B') {
            uint64_t avail = 0;
            sc_put("B "); sc_num(sys_fs_avail(-1, &avail));
        } else {
            continue;
        }
        sc_put("\n");
    }
    sc_flush();
    return 0;
}
