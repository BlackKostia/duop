/* src/fs/arc_path.c - writing below a destination without ever following a symlink (freestanding C23).
 *
 * A path from an archive is walked one component at a time with openat(O_NOFOLLOW | O_DIRECTORY) from the destination descriptor, so a
 * symlink in the way - one the archive created earlier or one that was already there - ends the walk with an error instead of
 * redirecting the write. Missing folders are made on the way (mkdirat, then opened the same strict way). */
#include "duop_arcunpack_int.h"
#include "duop_pal.h"
#include "duop_string.h"

#define COMP_MAX 255

intptr_t arc_dest_parent(int root, const char *rel, bool create, const char **leaf)
{
    int cur = root;
    bool owned = false;
    const char *p = rel;
    for (;;) {
        const char *slash = str_chr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : str_len(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            if (owned) (void)os_close(cur);
            return -EINVAL;
        }
        if (n > COMP_MAX) {
            if (owned) (void)os_close(cur);
            return -ENAMETOOLONG;
        }
        if (!slash) break;                                                     /* the last component is the leaf */
        char comp[COMP_MAX + 1];
        __builtin_memcpy(comp, p, n);
        comp[n] = 0;
        intptr_t fd = os_openat(cur, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        if (fd == -ENOENT && create) {
            intptr_t m = os_mkdirat(cur, comp, 0755);
            if (m == 0 || m == -EEXIST) fd = os_openat(cur, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
            else fd = m;
        }
        if (owned) (void)os_close(cur);
        if (os_is_err(fd)) return fd;
        cur = (int)fd;
        owned = true;
        p = slash + 1;
    }
    *leaf = p;
    if (owned) return cur;
    return os_openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);       /* no folder part: a descriptor of the root itself */
}

bool arc_link_safe(const char *rel, const char *target)
{
    if (!target[0] || target[0] == '/') return false;
    int depth = 0;                                                             /* folders between the root and the link's own folder */
    for (const char *p = rel; (p = str_chr(p, '/')); p++) depth++;
    for (const char *p = target; *p;) {
        const char *slash = str_chr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : str_len(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (--depth < 0) return false;
        } else if (n && !(n == 1 && p[0] == '.')) {
            depth++;
        }
        p += n + (slash ? 1 : 0);
    }
    return true;
}
