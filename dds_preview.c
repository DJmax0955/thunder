#include "dds_preview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static char *dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static char *strdup_fmt(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return dup_str(buf);
}

static uint32_t read_le32(const unsigned char *d, size_t off) {
    return (uint32_t)d[off] | ((uint32_t)d[off+1]<<8) | ((uint32_t)d[off+2]<<16) | ((uint32_t)d[off+3]<<24);
}

static void rgb565_to_rgb888(uint16_t c, unsigned char *r, unsigned char *g, unsigned char *b) {
    *r = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
    *g = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
    *b = (unsigned char)((c & 0x1F) * 255 / 31);
}

/* Same DXT1/DXT5 block decode math used (and verified byte-exact
 * against the Python reference) elsewhere in this project. */
static void decode_dxt1_block_rgb(const unsigned char *data, size_t offset, unsigned char out_rgb[16][3]) {
    uint16_t c0 = (uint16_t)(data[offset] | (data[offset+1]<<8));
    uint16_t c1 = (uint16_t)(data[offset+2] | (data[offset+3]<<8));
    uint32_t idx = (uint32_t)data[offset+4] | ((uint32_t)data[offset+5]<<8) |
                   ((uint32_t)data[offset+6]<<16) | ((uint32_t)data[offset+7]<<24);
    unsigned char col0[3], col1[3];
    rgb565_to_rgb888(c0, &col0[0], &col0[1], &col0[2]);
    rgb565_to_rgb888(c1, &col1[0], &col1[1], &col1[2]);
    unsigned char colors[4][3];
    memcpy(colors[0], col0, 3);
    memcpy(colors[1], col1, 3);
    if (c0 > c1) {
        for (int k=0;k<3;k++) colors[2][k] = (unsigned char)((2*col0[k]+col1[k])/3);
        for (int k=0;k<3;k++) colors[3][k] = (unsigned char)((col0[k]+2*col1[k])/3);
    } else {
        for (int k=0;k<3;k++) colors[2][k] = (unsigned char)((col0[k]+col1[k])/2);
        colors[3][0]=colors[3][1]=colors[3][2]=0;
    }
    for (int i=0;i<16;i++) { int sel=(idx>>(2*i))&0x3; memcpy(out_rgb[i], colors[sel], 3); }
}

static void decode_dxt_block_to_rgba(const unsigned char *data, size_t offset, int is_dxt5,
                                      unsigned char out[16][4]) {
    unsigned char rgb[16][3];
    unsigned char alpha[16];
    if (is_dxt5) {
        unsigned char a0 = data[offset], a1 = data[offset+1];
        uint64_t alpha_bits = 0;
        for (int k=0;k<6;k++) alpha_bits |= ((uint64_t)data[offset+2+k]) << (8*k);
        unsigned char alphas[8]; alphas[0]=a0; alphas[1]=a1;
        if (a0 > a1) { for (int i=1;i<7;i++) alphas[i+1]=(unsigned char)(((7-i)*a0+i*a1)/7); }
        else { for (int i=1;i<5;i++) alphas[i+1]=(unsigned char)(((5-i)*a0+i*a1)/5); alphas[6]=0; alphas[7]=255; }
        for (int i=0;i<16;i++) alpha[i] = alphas[(alpha_bits>>(3*i))&0x7];
        decode_dxt1_block_rgb(data, offset+8, rgb);
    } else {
        decode_dxt1_block_rgb(data, offset, rgb);
        for (int i=0;i<16;i++) alpha[i]=255;
    }
    for (int i=0;i<16;i++) {
        out[i][0]=rgb[i][0]; out[i][1]=rgb[i][1]; out[i][2]=rgb[i][2]; out[i][3]=alpha[i];
    }
}

int dds_preview_load(const char *path, unsigned char **out_rgba,
                      uint32_t *out_w, uint32_t *out_h, char **out_error) {
    *out_error = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        *out_error = strdup_fmt("error: could not open '%s'.", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 128) {
        fclose(f);
        *out_error = strdup_fmt("error: '%s' is too small to be a DDS file.", path);
        return -1;
    }
    unsigned char *data = (unsigned char *)malloc((size_t)size);
    size_t read_count = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (read_count != (size_t)size || memcmp(data, "DDS ", 4) != 0) {
        free(data);
        *out_error = strdup_fmt("error: '%s' is not a valid DDS file.", path);
        return -1;
    }

    uint32_t h = read_le32(data, 12);
    uint32_t w = read_le32(data, 16);
    uint32_t pf_flags = read_le32(data, 80);

    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        free(data);
        *out_error = strdup_fmt("error: '%s' has implausible dimensions (%ux%u).", path, w, h);
        return -1;
    }

    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);

    if (pf_flags & 0x4) {
        int is_dxt1 = memcmp(data + 84, "DXT1", 4) == 0;
        int is_dxt5 = memcmp(data + 84, "DXT5", 4) == 0;
        if (!is_dxt1 && !is_dxt5) {
            free(data); free(rgba);
            *out_error = strdup_fmt("error: '%s' uses an unrecognized compressed FourCC.", path);
            return -1;
        }
        int block_bytes = is_dxt1 ? 8 : 16;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        size_t payload_len = (size_t)bw * bh * (size_t)block_bytes;
        if (128 + payload_len > (size_t)size) {
            free(data); free(rgba);
            *out_error = strdup_fmt("error: '%s' texture data is truncated.", path);
            return -1;
        }
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t off = 128 + ((size_t)by * bw + bx) * (size_t)block_bytes;
                unsigned char block[16][4];
                decode_dxt_block_to_rgba(data, off, is_dxt5, block);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx*4 + (i%4), y = by*4 + (i/4);
                    if (x < w && y < h) {
                        size_t pidx = ((size_t)y*w+x)*4;
                        memcpy(rgba+pidx, block[i], 4);
                    }
                }
            }
        }
    } else {
        size_t expected = (size_t)w * h * 4;
        if (128 + expected > (size_t)size) {
            free(data); free(rgba);
            *out_error = strdup_fmt("error: '%s' declares %ux%u RGBA but doesn't have enough pixel data.", path, w, h);
            return -1;
        }
        /* Respect the header's own declared channel masks rather than
         * assuming one fixed byte order -- this project's own DDS
         * writers aren't consistent with each other here (x360/ps3
         * write literal R,G,B,A byte order; wii_texpack.c writes
         * B,G,R,A), and any standard DDS from elsewhere is free to use
         * either. Windows' own DDS thumbnail handler reads these masks
         * too, which is why Explorer showed the correct color while
         * this decoder (before this fix) didn't. */
        uint32_t red_mask = read_le32(data, 92);
        int bgr_order = (red_mask == 0x00FF0000u);
        const unsigned char *src = data + 128;
        if (bgr_order) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                rgba[i*4+0] = src[i*4+2]; /* R <- byte 2 */
                rgba[i*4+1] = src[i*4+1]; /* G <- byte 1 */
                rgba[i*4+2] = src[i*4+0]; /* B <- byte 0 */
                rgba[i*4+3] = src[i*4+3]; /* A <- byte 3 */
            }
        } else {
            memcpy(rgba, src, expected);
        }
    }

    free(data);
    *out_rgba = rgba;
    *out_w = w;
    *out_h = h;
    return 0;
}

void dds_preview_free(void *p) {
    free(p);
}
