#ifndef DDS_PREVIEW_H
#define DDS_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generic DDS -> RGBA decoder, for thumbnail preview purposes. Platform
 * (PS3/X360) agnostic -- reads any DDS this app itself would produce:
 * DXT1, DXT5, or uncompressed 32-bit RGBA. Only decodes mip0 (the first
 * face, if it's a cubemap -- fine for a thumbnail).
 *
 * Uses the same DXT1/DXT5 block-decode math already verified byte-exact
 * against the real Python reference decoders elsewhere in this project.
 *
 * path        : path to the .dds file.
 * out_rgba    : on success, a malloc'd width*height*4 byte buffer
 *               (caller frees with dds_preview_free).
 * out_w/out_h : mip0's pixel dimensions.
 * out_error   : on failure, a malloc'd error string (caller frees).
 *               NULL on success.
 *
 * Returns 0 on success, non-zero on failure.
 */
int dds_preview_load(const char *path, unsigned char **out_rgba,
                      uint32_t *out_w, uint32_t *out_h, char **out_error);

void dds_preview_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* DDS_PREVIEW_H */
