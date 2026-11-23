/* Freestanding harness for the archive parsers (src/fs/arc_tar.c, arc_zip.c). Script on stdin:
 *   T <path>   parse a tar with arc_tar_parse          Z <path>   parse a zip with arc_zip_parse
 * The file is copied to the END of a mapping with a PROT_NONE page on both sides, so a read before or behind it faults; the parser runs
 * under the fault guard. Output: "<kind> rc=<r> n=<unique> dmg=<damaged> skip=<skipped> uns=<unsupported> enc=<encrypted>" or "<kind> fault",
 * then one line per distinct name in order: "<hexname> <flags> <size> <comp_size> <off> <mtime> <mode> <method> <crc> <hexlink|->". */
#include "script_cli.h"
#include "duop_arcidx.h"
#include "duop_arcparse.h"
#include "duop_pal.h"

static void put_hex(const char *s)
{
    for (; *s; s++) {
        char t[3] = {"0123456789abcdef"[(uint8_t)*s >> 4], "0123456789abcdef"[(uint8_t)*s & 15], 0};
        sc_put(t);
    }
}

static void dump(const ArcIndex *x)
{
    for (uint32_t i = 0; i < x->unique; i++) {
        const ArcItem *it = &x->items[x->order[i]];
        put_hex(arc_item_name(x, it));
        sc_put(" "); sc_num(it->flags); sc_put(" "); sc_num((int64_t)it->size); sc_put(" "); sc_num((int64_t)it->comp_size);
        sc_put(" "); sc_num((int64_t)it->off); sc_put(" "); sc_num(it->mtime); sc_put(" "); sc_num(it->mode);
        sc_put(" "); sc_num(it->method); sc_put(" "); sc_num(it->crc32); sc_put(" ");
        const char *l = arc_item_link(x, it);
        if (l) put_hex(l); else sc_put("-");
        sc_put("\n");
    }
}

static intptr_t do_parse(char kind, ArcIndex *x, const uint8_t *d, size_t n, ArcParseInfo *info)
{
#ifdef WITH_TAR
    if (kind == 'T') return arc_tar_parse(x, d, n, info);
#endif
#ifdef WITH_ZIP
    if (kind == 'Z') return arc_zip_parse(x, d, n, info);
#endif
    (void)x; (void)d; (void)n; (void)info;
    return -EOPNOTSUPP;
}

static void parse_file(char kind, const char *path)
{
    intptr_t fd = os_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    FileStat st;
    if (os_is_err(fd) || os_fstat((int)fd, &st) < 0) { sc_put("open failed\n"); return; }
    size_t len = (size_t)st.size, total = (len + 4095) & ~(size_t)4095;
    intptr_t m = os_mmap(nullptr, total + 8192, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
    if (os_is_err(m) || os_is_err(os_mmap((void *)m, 4096, 0, MAP_PRIVATE_ANON | MAP_FIXED, -1, 0)) ||
        os_is_err(os_mmap((void *)(m + 4096 + (intptr_t)total), 4096, 0, MAP_PRIVATE_ANON | MAP_FIXED, -1, 0))) { sc_put("map failed\n"); return; }
    uint8_t *data = (uint8_t *)m + 4096 + total - len;
    for (size_t got = 0; got < len;) {
        intptr_t r = os_read((int)fd, data + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    (void)os_close((int)fd);
    static ArcIndex x;
    arc_idx_init(&x);
    ArcParseInfo info = {0};
    volatile intptr_t rc = 0;
    TryFrame f;
    if (os_try_begin(&f) == 0) {
        rc = do_parse(kind, &x, data, len, &info);
        os_try_end(&f);
    } else {
        sc_put(kind == 'T' ? "T fault\n" : "Z fault\n");
        arc_idx_free(&x);
        (void)os_munmap_raw((void *)m, total + 8192);
        return;
    }
    (void)arc_idx_finish(&x);
    char head[2] = {kind, 0};
    sc_put(head); sc_put(" rc="); sc_num(rc); sc_put(" n="); sc_num(x.unique); sc_put(" dmg="); sc_num(info.damaged);
    sc_put(" skip="); sc_num(info.skipped); sc_put(" uns="); sc_num(info.unsupported); sc_put(" enc="); sc_num(info.encrypted); sc_put("\n");
    dump(&x);
    arc_idx_free(&x);
    (void)os_munmap_raw((void *)m, total + 8192);
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));)
        if ((line[0] == 'T' || line[0] == 'Z') && line[1] == ' ') parse_file(line[0], line + 2);
    sc_flush();
    return 0;
}
