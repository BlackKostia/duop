/* Freestanding harness for src/fs/arc_index.c. Script on stdin, names hex-encoded (so spaces, UTF-8 and control bytes survive):
 *   A <hexname> <hexlink|-> <flags> <size> <off>   arc_idx_add;  prints the result code
 *   F                                              arc_idx_finish: prints the result and the number of names kept
 *   G <hexpath>                                    arc_idx_find:  "none" or "<size> <flags> <off> <link-hex>"
 *   L <hexprefix>                                  arc_idx_list:  the children in order, "name:d|f:size ..."
 *   S                                              items / unique / unsafe counters
 *   X <n>                                          arc_idx_add of n synthetic entries "d<i%50>/f<i>" (growth, no output per entry) */
#include "script_cli.h"
#include "duop_arcidx.h"

static ArcIndex g_x;

static size_t unhex(const char *h, char *out, size_t cap)
{
    size_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        uint64_t v;
        char t[3] = {h[0], h[1], 0};
        (void)sc_hex(t, &v);
        out[n++] = (char)v;
    }
    return n;
}

static void put_hex(const char *s)
{
    static const char d[] = "0123456789abcdef";
    for (; *s; s++) {
        char t[3] = {d[(uint8_t)*s >> 4], d[(uint8_t)*s & 15], 0};
        sc_put(t);
    }
}

static int list_cb(void *ctx, const char *comp, size_t clen, bool is_dir, const ArcItem *it)
{
    (void)ctx;
    sc_put(" ");
    for (size_t i = 0; i < clen; i++) {
        char t[3] = {"0123456789abcdef"[(uint8_t)comp[i] >> 4], "0123456789abcdef"[(uint8_t)comp[i] & 15], 0};
        sc_put(t);
    }
    sc_put(is_dir ? ":d:" : ":f:");
    sc_num(it ? (int64_t)it->size : -1);
    return 0;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    static char name[70000], link[70000];
    char *script = sc_read_all();
    arc_idx_init(&g_x);
    for (char *line; (line = sc_next_line(&script));) {
        char cmd = line[0];
        const char *p = line + 1;
        while (*p == ' ') p++;
        if (cmd == 'A') {
            char hn[140100], hl[140100];
            size_t k = 0;
            while (*p && *p != ' ') hn[k++] = *p++;
            hn[k] = 0;
            while (*p == ' ') p++;
            k = 0;
            while (*p && *p != ' ') hl[k++] = *p++;
            hl[k] = 0;
            uint64_t flags, size, off;
            p = sc_dec(p, &flags);
            p = sc_dec(p, &size);
            p = sc_dec(p, &off);
            size_t nl = unhex(hn, name, sizeof name), ll = hl[0] == '-' ? 0 : unhex(hl, link, sizeof link);
            ArcItem proto = {0};
            proto.flags = (uint16_t)flags;
            proto.size = size;
            proto.off = off;
            int r = arc_idx_add(&g_x, name, nl, hl[0] == '-' ? nullptr : link, ll, &proto);
            sc_put("A "); sc_num(r); sc_put("\n");
        } else if (cmd == 'F') {
            int r = arc_idx_finish(&g_x);
            sc_put("F "); sc_num(r); sc_put(" "); sc_num(g_x.unique); sc_put("\n");
        } else if (cmd == 'G') {
            size_t nl = unhex(p, name, sizeof name);
            name[nl] = 0;
            const ArcItem *it = arc_idx_find(&g_x, name);
            if (!it) sc_put("none\n");
            else {
                sc_num((int64_t)it->size); sc_put(" "); sc_num(it->flags); sc_put(" "); sc_num((int64_t)it->off); sc_put(" ");
                const char *l = arc_item_link(&g_x, it);
                if (l) put_hex(l); else sc_put("-");
                sc_put("\n");
            }
        } else if (cmd == 'L') {
            size_t nl = unhex(p, name, sizeof name);
            name[nl] = 0;
            sc_put("L");
            int r = arc_idx_list(&g_x, name, list_cb, nullptr);
            sc_put(" r="); sc_num(r); sc_put("\n");
        } else if (cmd == 'S') {
            sc_num(g_x.count); sc_put(" "); sc_num(g_x.unique); sc_put(" "); sc_num(g_x.unsafe); sc_put("\n");
        } else if (cmd == 'X') {
            uint64_t n;
            (void)sc_dec(p, &n);
            int bad = 0;
            for (uint64_t i = 0; i < n; i++) {
                char nm[32];
                size_t k = 0;
                nm[k++] = 'd';
                k += u64_to_dec(i % 50, nm + k);
                nm[k++] = '/';
                nm[k++] = 'f';
                k += u64_to_dec(i, nm + k);
                ArcItem pr = {0};
                pr.size = i;
                if (arc_idx_add(&g_x, nm, k, nullptr, 0, &pr) != 0) bad++;
            }
            sc_put("X "); sc_num(bad); sc_put("\n");
        }
    }
    arc_idx_free(&g_x);
    sc_flush();
    return 0;
}
