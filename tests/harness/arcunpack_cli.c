/* Freestanding harness for arc_unpack / arc_test (src/fs/arc_unpack*.c). Script on stdin, blank-separated tokens:
 *   U <archive> <dst> <policy> <smart> <bomb_floor> <bomb_ratio> <max_bytes> <folder|-> [member ...]
 *        arc_unpack; policy 0 / 1 overwrite / 2 skip / 3 rename (CONFLICT_ACT_*); no members = everything
 *        -> "U rc=<r> files=<n> dirs=<n> links=<n> bytes=<n> skipped=<n> kept=<n> err=<first_error> dmg=<damaged>"
 *   T <archive>   arc_test -> "T rc=<r> files=<n> bytes=<n> skipped=<n> err=<first_error> dmg=<damaged>" */
#include "script_cli.h"
#include "duop_arcunpack.h"
#include "duop_pal.h"

static int tokens(char *line, char **tok, int max)
{
    int n = 0;
    while (*line && n < max) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        tok[n++] = line;
        while (*line && *line != ' ') line++;
    }
    return n;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    static char names[1 << 20];
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *t[512];
        int n = tokens(line, t, 512);
        if (n < 2) continue;
        if (t[0][0] == 'U' && n >= 9) {
            uint64_t policy, smart, floor_, ratio, cap;
            (void)sc_dec(t[3], &policy); (void)sc_dec(t[4], &smart); (void)sc_dec(t[5], &floor_); (void)sc_dec(t[6], &ratio); (void)sc_dec(t[7], &cap);
            ArcUnpackOpts o = {(uint32_t)policy, (uint32_t)smart, cap, floor_, ratio};
            size_t k = 0;
            const char *folder = t[8][0] == '-' && !t[8][1] ? "" : t[8];
            for (const char *p = folder; *p; p++) names[k++] = *p;
            names[k++] = 0;
            for (int i = 9; i < n; i++) {
                for (const char *p = t[i]; *p; p++) names[k++] = *p;
                names[k++] = 0;
            }
            ArcUnpackReport r = {0};
            intptr_t rc = arc_unpack(t[1], t[2], names, (uint32_t)(n - 9), &o, &r);
            sc_put("U rc="); sc_num(rc); sc_put(" files="); sc_num((int64_t)r.files); sc_put(" dirs="); sc_num((int64_t)r.dirs);
            sc_put(" links="); sc_num((int64_t)r.links); sc_put(" bytes="); sc_num((int64_t)r.bytes); sc_put(" skipped="); sc_num((int64_t)r.skipped);
            sc_put(" kept="); sc_num((int64_t)r.kept); sc_put(" err="); sc_num(r.first_error); sc_put(" dmg="); sc_num(r.damaged); sc_put("\n");
        } else if (t[0][0] == 'T') {
            ArcUnpackReport r = {0};
            intptr_t rc = arc_test(t[1], &r);
            sc_put("T rc="); sc_num(rc); sc_put(" files="); sc_num((int64_t)r.files); sc_put(" bytes="); sc_num((int64_t)r.bytes);
            sc_put(" skipped="); sc_num((int64_t)r.skipped); sc_put(" err="); sc_num(r.first_error); sc_put(" dmg="); sc_num(r.damaged); sc_put("\n");
        }
        sc_flush();
    }
    sc_flush();
    return 0;
}
