#ifndef TSZIP_WRITER_H
#define TSZIP_WRITER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Builds a .tszip matching make_tszip_v3_fixed.py's exact byte layout:
 * [EOCD][CD][body][CD][EOCD], STORED entries, absolute local-header
 * offsets, MD5 extra fields. Verified spec-compliant by running the
 * actual reference script's own validate_tszip() against our output.
 *
 * Shared by both ps3_texpack.c and x360_texpack.c's repack functions --
 * both platforms package streamed textures' data into a .tszip archive
 * this same way.
 */

typedef struct {
    char name[512];
    unsigned char *data;
    size_t size;
} TszipEntry;

/* Returns 0 on success, non-zero on failure. */
int build_tszip(const TszipEntry *entries, int count, const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* TSZIP_WRITER_H */
