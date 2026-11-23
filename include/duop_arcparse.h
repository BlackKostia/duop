/* include/duop_arcparse.h - the archive format parsers (src/fs/arc_tar.c, src/fs/arc_zip.c; docs/ARCHIVE_TOOLS_PLAN.md, phase 1).
 *
 * The input is untrusted and wholly in memory (an mmap). Every offset and length is compared as "does it fit in what remains" - no
 * sum that could wrap is ever formed - and the parsers only read inside [data, data + len). They fill an ArcIndex (arc_idx_finish is
 * the caller's) and report what they could not use in ArcParseInfo. */
#pragma once
#include "duop_arcidx.h"

typedef struct ArcParseInfo {
    uint32_t damaged;               /* the archive ends early or a header is invalid: what was read before is in the index */
    uint32_t skipped;               /* members that could not be listed (name too long, empty, NUL in the name) */
    uint32_t unsupported;           /* members using a feature that is not implemented (sparse files, multi-disk) */
    uint32_t encrypted;             /* zip members with the encryption flag */
    uint64_t end;                   /* tar: offset of the end-of-archive block (or the data length) */
} ArcParseInfo;

/* 0, -EINVAL when data is not this kind of archive (the first header / the end record is invalid), -ENOMEM, -EFBIG (too many members) */
int arc_tar_parse(ArcIndex *x, const uint8_t *data, uint64_t len, ArcParseInfo *info);
int arc_zip_parse(ArcIndex *x, const uint8_t *data, uint64_t len, ArcParseInfo *info);
