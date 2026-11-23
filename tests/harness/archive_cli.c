/* Freestanding harness: the frozen asm archive parsers (archive_vfs_reference.s, x86_64, ref_ prefix) or src/fs/archive_parse.c (compiled
 * into this unit, path in AR_SRC1). The virtual folder is tested in tests/test_c_arc_vfs.py (harness arcvfs_cli.c).
 * argv[1] = ref | c. Script on stdin:
 *   P <file> <t|z|g|Z>  parse a whole file (tar / zip index, or gunzip): the entries (or the gunzipped length and hash); a fault
 *                       inside the parser prints "fault" (the fault guard is installed); Z = a zip right behind a PROT_NONE page */
#include "script_cli.h"
#include "duop_arc.h"
#include "duop_pal.h"

/* the frozen asm object still links the DirModel stack of its virtual folder (unused here) */
int git_refresh(DirModel *m) { (void)m; return 0; }
void git_free_ctx(DirModel *m) { m->git_ctx = nullptr; }

#include AR_SRC1

#ifdef __x86_64__
extern void ref_arc_parse_tar(ArcCtx *), ref_arc_parse_zip(ArcCtx *);
extern void *ref_arc_ungzip(const uint8_t *, uint64_t, uint64_t *);
extern int ref_arc_is_archive(const char *);
#define CALL(f, ...) (g_ref ? ref_##f(__VA_ARGS__) : f(__VA_ARGS__))
#define CALLV(f, ...) do { if (g_ref) ref_##f(__VA_ARGS__); else f(__VA_ARGS__); } while (0)
#else
#define CALL(f, ...) f(__VA_ARGS__)
#define CALLV(f, ...) f(__VA_ARGS__)
#endif

static bool g_ref;
static int sc_split(char *line, char **f, int max)
{
    int n = 0;
    while (*line && n < max) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        f[n++] = line;
        while (*line && *line != ' ') line++;
    }
    return n;
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h >> 1;
}

static void cmd_parse(const char *file, char kind_arg)
{
    volatile char kind = kind_arg;
    ArcCtx *ctx = mem_alloc(sizeof *ctx);
    if (!ctx) return;
    mem_zero(ctx, sizeof *ctx);
    /* kind 'Z': a zip whose buffer starts right after a PROT_NONE page, so a read before the buffer faults */
    uint8_t *volatile guard = nullptr;
    if (kind == 'Z') {
        intptr_t g = os_mmap(nullptr, ARC_MAX_FILE + 4096, PROT_RW, MAP_PRIVATE_ANON, -1, 0);
        if (os_is_err(g) || os_is_err(os_mmap((void *)g, 4096, 0, MAP_PRIVATE_ANON | MAP_FIXED, -1, 0))) { sc_put("P noread"); return; }
        guard = (uint8_t *)g + 4096;
        kind = 'z';
    }
    ctx->raw = guard ? guard : mem_alloc(ARC_MAX_FILE);
    ctx->entries = mem_alloc(ARC_MAX_ENTRIES * sizeof(ArcEntry));
    size_t got = 0;
    if (!ctx->raw || !ctx->entries || fs_read_file_head(file, ctx->raw, ARC_MAX_FILE, &got) < 0) { sc_put("P noread"); return; }
    ctx->raw_len = got;
    mem_zero(ctx->entries, ARC_MAX_ENTRIES * sizeof(ArcEntry));
    sc_put("P");
    volatile uint64_t outlen = 0;
    void *volatile out = nullptr;
    TryFrame f;
    if (os_try_begin(&f) == 0) {
        uint64_t n = 0;
        if (kind == 't') CALLV(arc_parse_tar, ctx);
        else if (kind == 'z') CALLV(arc_parse_zip, ctx);
        else { out = CALL(arc_ungzip, ctx->raw, ctx->raw_len, &n); outlen = n; }
        os_try_end(&f);
    } else {
        sc_put(" fault");
        if (guard) (void)os_munmap_raw(guard - 4096, ARC_MAX_FILE + 4096);
        return;
    }
    if (kind == 'g') {
        sc_put(" gz="); sc_num(out != nullptr); sc_put(" len="); sc_num((int64_t)outlen);
        if (out) { sc_put(" h="); sc_num((int64_t)fnv(out, outlen)); mem_free(out); }
        mem_free(ctx->raw); mem_free(ctx->entries); mem_free(ctx);
        return;
    }
    sc_put(" n="); sc_num(ctx->count);
    for (uint32_t i = 0; i < ctx->count; i++) {
        const ArcEntry *e = &ctx->entries[i];
        sc_put(" ["); sc_esc(e->name); sc_put(" "); sc_num((int64_t)e->off); sc_put(" "); sc_num((int64_t)e->size); sc_put(" ");
        sc_num((int64_t)e->comp_size); sc_put(" "); sc_num(e->method); sc_put(" "); sc_num(e->is_dir); sc_put("]");
    }
    if (guard) (void)os_munmap_raw(guard - 4096, ARC_MAX_FILE + 4096);
    else mem_free(ctx->raw);
    mem_free(ctx->entries); mem_free(ctx);
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)envp; (void)auxv;
    if (argc < 2) return 2;
    g_ref = argv[1][0] == 'r';
    os_fault_guard_install();
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *f[4];
        int n = sc_split(line, f, 4);
        if (!n) continue;
        switch (f[0][0]) {
        case 'P': if (n < 3) continue; cmd_parse(f[1], f[2][0]); break;
        default: continue;
        }
        sc_put("\n");
        sc_flush();
    }
    sc_flush();
    return 0;
}
