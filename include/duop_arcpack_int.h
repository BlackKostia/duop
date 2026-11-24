/* include/duop_arcpack_int.h - what the files of the archive writer share (src/fs/arc_pack.c: walking and output, tar_write.c, zip_write.c). */
#pragma once
#include "duop_arcpack.h"
#include "duop_codec.h"
#include "duop_config.h"
#include "duop_pal.h"
#include "duop_deflate.h"

#ifndef ARC_PACK_ZIP64_AT
#define ARC_PACK_ZIP64_AT 0xFFFF0000ull        /* an entry this big (or bigger) gets ZIP64 records; tests lower it */
#endif

typedef struct PackEntry {
    const char *name;                  /* path inside the archive, '/' separated; a folder's ends with '/' */
    const char *link;                  /* symlink target, else NULL */
    uint64_t size;                     /* bytes of data to come (0 for folders and links in a tar) */
    int64_t mtime;
    uint32_t mode;                     /* permission bits */
    uint32_t uid, gid;
    char type;                         /* '0' file, '5' folder, '2' symlink */
} PackEntry;

/* ---- tar (tar_write.c): ustar headers, PAX extended headers for what ustar cannot say ---- */
typedef struct TarWriter {
    const CodecSink *out;
    uint64_t left;                     /* data bytes still due for the current entry */
    uint64_t size;                     /* its declared size, for the padding */
    char pax[2 * 4096 + 512];
} TarWriter;
int tar_begin(TarWriter *t, const PackEntry *e);
int tar_data(TarWriter *t, const uint8_t *p, size_t n);
int tar_end(TarWriter *t);             /* zeros for data that did not come, then the padding to 512 */
int tar_finish(TarWriter *t);          /* the two zero blocks */

/* ---- zip (zip_write.c): streamed entries with data descriptors, central directory at the end ---- */
typedef struct ZipWriter ZipWriter;
ZipWriter *zip_new(const CodecSink *out, int level);
int zip_begin(ZipWriter *z, const PackEntry *e);
int zip_data(ZipWriter *z, const uint8_t *p, size_t n);
int zip_end(ZipWriter *z);
int zip_finish(ZipWriter *z);
void zip_free(ZipWriter *z);

/* ---- the output of a pack run (arc_pack_out.c): a temporary file next to dst, an optional gzip layer, commit by rename ---- */
typedef struct PackOut {
    int fd;
    char part[PATH_MAX], dst[PATH_MAX];
    uint64_t dev, ino;                 /* of the temporary file: it is never packed into itself */
    uint64_t bytes_out;
    int err;                           /* the first failure of the sink, kept for the caller */
    CodecSink file;                    /* raw bytes to the file */
    CodecSink top;                     /* where the tar / zip writer writes: the file, or the gzip layer */
    Deflate *def;
    CodecSink gz_in, gz_out;
    uint32_t crc;
    uint64_t usize;
    bool gz_started;
} PackOut;
int pack_out_open(PackOut *o, const char *dst, uint32_t format, uint32_t level);      /* -EEXIST when dst exists, -errno when the file cannot be made */
int pack_out_close(PackOut *o, bool commit);                                         /* gzip trailer, fsync, rename (commit) or removal */
