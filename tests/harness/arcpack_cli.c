/* Freestanding harness for arc_pack (src/fs/arc_pack*.c, tar_write.c, zip_write.c). Script on stdin, TAB-separated:
 *   P <dst> <format 0 tar / 1 tgz / 2 zip> <level> <flags> <mtime> <base> <name> [<name> ...]
 *      -> "P rc=<r> files=<n> dirs=<n> links=<n> bytes=<in> skipped=<n> changed=<n> err=<first error> out=<bytes written>"
 *   H <size> <uid> <mtime> <type> <mode> <name> <link>   one tar header with no data (the sizes the writer must express) -> the header bytes as hex */
#include "script_cli.h"
#include "duop_arcpack.h"
#include "duop_pal.h"

static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    while (*line && n < max) {
        f[n++] = line;
        while (*line && *line != '\t') line++;
        if (*line) *line++ = 0;
    }
    return n;
}

static uint64_t num(const char *s)
{
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint64_t)(*s - '0');
    return v;
}

static int64_t snum(const char *s) { return *s == '-' ? -(int64_t)num(s + 1) : (int64_t)num(s); }

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    os_fault_guard_install();
    static char names[1 << 20];
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *f[256];
        int n = split_tabs(line, f, 256);
        if (n >= 8 && f[0][0] == 'P') {
            ArcPackOpts o = {(uint32_t)num(f[2]), (uint32_t)num(f[3]), (uint32_t)num(f[4]), 0, (int64_t)num(f[5])};
            size_t k = 0;
            for (const char *p = f[6]; *p; p++) names[k++] = *p;
            names[k++] = 0;
            for (int i = 7; i < n; i++) {
                for (const char *p = f[i]; *p; p++) names[k++] = *p;
                names[k++] = 0;
            }
            ArcPackReport r = {0};
            intptr_t rc = arc_pack(f[1], names, (uint32_t)(n - 7), &o, &r);
            sc_put("P rc="); sc_num(rc); sc_put(" files="); sc_num((int64_t)r.files); sc_put(" dirs="); sc_num((int64_t)r.dirs);
            sc_put(" links="); sc_num((int64_t)r.links); sc_put(" bytes="); sc_num((int64_t)r.bytes_in); sc_put(" skipped="); sc_num((int64_t)r.skipped);
            sc_put(" changed="); sc_num((int64_t)r.changed); sc_put(" err="); sc_num(r.first_error); sc_put(" out="); sc_num((int64_t)r.bytes_out); sc_put("\n");
        } else if (n >= 7 && f[0][0] == 'H') {
            uint8_t hdr[2048];
            size_t len = arc_pack_tar_header_test(hdr, sizeof hdr, num(f[1]), (uint32_t)num(f[2]), snum(f[3]), f[4][0], (uint32_t)num(f[5]), f[6], n >= 8 ? f[7] : "");
            static const char hex[] = "0123456789abcdef";
            sc_put("H ");
            for (size_t i = 0; i < len; i++) {
                char t[3] = {hex[hdr[i] >> 4], hex[hdr[i] & 15], 0};
                sc_put(t);
            }
            sc_put("\n");
        }
        sc_flush();
    }
    sc_flush();
    return 0;
}
