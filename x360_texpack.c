#include "x360_texpack.h"
#include "tszip_reader.h"
#include "tszip_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#if defined(_WIN32)
#include <direct.h>
#define X360_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define X360_MKDIR(path) mkdir(path, 0755)
#endif

/* --------------------------------------------------------------------
 * Small helpers -- same shape as ps3_texpack.c's (log buffer, string
 * formatting, big/little-endian reads, whole-file reading, path utils).
 * NOTE: %lu (with an explicit (unsigned long) cast), never %zu -- some
 * Windows/MinGW C runtimes mishandle %zu and corrupt the stack instead
 * of just failing to print (this bit us for real in ps3_texpack.c).
 * ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} LogBuf;

static void log_init(LogBuf *lb) {
    lb->data = (char *)malloc(1);
    lb->data[0] = '\0';
    lb->len = 0;
    lb->cap = 1;
}

static void log_append(LogBuf *lb, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    size_t line_len = strlen(line);
    size_t needed = lb->len + line_len + 2;
    if (needed > lb->cap) {
        size_t new_cap = lb->cap * 2;
        if (new_cap < needed)
            new_cap = needed;
        lb->data = (char *)realloc(lb->data, new_cap);
        lb->cap = new_cap;
    }
    memcpy(lb->data + lb->len, line, line_len);
    lb->len += line_len;
    lb->data[lb->len++] = '\n';
    lb->data[lb->len] = '\0';
}

static char *dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static char *strdup_fmt(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return dup_str(buf);
}

static uint32_t read_be32(const unsigned char *data, size_t offset) {
    return ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3];
}

static void write_le32(unsigned char *buf, size_t offset, uint32_t value) {
    buf[offset + 0] = (unsigned char)(value & 0xFF);
    buf[offset + 1] = (unsigned char)((value >> 8) & 0xFF);
    buf[offset + 2] = (unsigned char)((value >> 16) & 0xFF);
    buf[offset + 3] = (unsigned char)((value >> 24) & 0xFF);
}

static uint32_t read_le32(const unsigned char *data, size_t offset) {
    return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}

static int read_whole_file(const char *path, unsigned char **out_data, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    unsigned char *buf = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    if (!buf) { fclose(f); return -1; }

    size_t read_count = (size > 0) ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    if (read_count != (size_t)size) { free(buf); return -1; }

    *out_data = buf;
    *out_size = (size_t)size;
    return 0;
}

/* Finds a stream by exact "_<index>.tstream" SUFFIX match, ignoring
 * whatever precedes it -- not an exact full-name match against this
 * file's own derived prefix. Real files can share one .tszip across
 * resolution variants (confirmed on PS3 with "dolly_high.thb" pulling
 * from "DOLLY.TSZIP", whose entries are named "dolly_0.tstream"),
 * so matching by suffix alone is what actually works in practice. */
static int find_stream_by_suffix(const char *tszip_path, int index,
                                  unsigned char **out_data, size_t *out_size,
                                  char **out_name, char **out_error) {
    char **names = NULL; int n = 0; char *err = NULL;
    if (tszip_list_entries(tszip_path, &names, &n, &err) != 0) {
        if (out_error) *out_error = err; else free(err);
        return -1;
    }
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "_%d.tstream", index);
    size_t suffix_len = strlen(suffix);
    const char *found = NULL;
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(names[i]);
        if (nl >= suffix_len && strcmp(names[i] + nl - suffix_len, suffix) == 0) {
            found = names[i];
            break;
        }
    }
    int rc = -1;
    if (found) {
        char *read_err = NULL;
        if (tszip_read_entry(tszip_path, found, out_data, out_size, &read_err) == 0) {
            if (out_name) *out_name = dup_str(found);
            rc = 0;
        } else if (out_error) {
            *out_error = read_err ? read_err : strdup_fmt("unknown error");
            free(read_err);
        } else {
            free(read_err);
        }
    } else if (out_error) {
        *out_error = strdup_fmt("no entry ending in '%s' found inside '%s'.", suffix, tszip_path);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    return rc;
}

static void mkpath(const char *path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return;
    strcpy(buf, path);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i];
            buf[i] = '\0';
            X360_MKDIR(buf);
            buf[i] = c;
        }
    }
    X360_MKDIR(buf);
}

static void derive_prefix(const char *path, char *out, size_t out_size) {
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *name = path;
    if (slash1 && (!slash2 || slash1 > slash2))
        name = slash1 + 1;
    else if (slash2)
        name = slash2 + 1;

    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

static void derive_dir(const char *path, char *out, size_t out_size) {
    const char *slash1 = strrchr(path, '/');
    const char *slash2 = strrchr(path, '\\');
    const char *slash = slash1;
    if (slash2 && (!slash1 || slash2 > slash1))
        slash = slash2;
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* --------------------------------------------------------------------
 * Xenos tiling math -- ported field-for-field from
 * _xg_address_2d_tiled_offset()/unswizzle_mip()/unswizzle_mip_region()/
 * swap16() in x360_thb_tbb_extractv5.py.
 * ------------------------------------------------------------------ */

/* Swap adjacent byte pairs in place -- fixes the 360's big-endian word
 * order before any block data can be interpreted. */
static void swap16(unsigned char *data, size_t len) {
    size_t i;
    for (i = 0; i + 1 < len; i += 2) {
        unsigned char tmp = data[i];
        data[i] = data[i + 1];
        data[i + 1] = tmp;
    }
}

/* ARGB8 (0x86) is a 32bpp surface, not a block format: once untiled its
 * bytes already read A,R,G,B, so the 16-bit word swap must NOT be applied
 * to it (doing so yields R,A,B,G per pixel -- the cyan/magenta casts seen
 * on ingameicons). block_bytes == 4 is ARGB8; DXT1/DXT5/CTX1 are block
 * formats and do need the swap. Use this everywhere instead of calling
 * swap16() directly, on both the unpack and repack sides, so the two
 * stay symmetric. */
static void swap_elems(unsigned char *data, size_t len, int block_bytes) {
    if (block_bytes == 4)
        return;                 /* ARGB8: already in the right byte order */
    swap16(data, len);
}

/* Integer log2 of a power-of-two block size (8 or 16 in practice). */
static int bit_length_minus_1(int v) {
    int bits = 0;
    while (v > 1) { v >>= 1; bits++; }
    return bits;
}

/* Microsoft XGAddress2DTiledOffset -- the real Xenos 2D texture tiler.
 * Returns the storage element INDEX (not byte offset) for a block at
 * (x, y). Ported directly from the Python; every operator/shift/mask
 * matches exactly, just typed as uint32_t throughout for portability. */
static uint32_t xg_address_2d_tiled_offset(uint32_t x, uint32_t y,
                                            uint32_t blocks_wide, int block_bytes) {
    int log_bpp = bit_length_minus_1(block_bytes);
    uint32_t aligned_width = (blocks_wide + 31u) & ~31u;
    uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7);
    uint32_t micro = (((x & 7u) + ((y & 6u) << 2)) << log_bpp);
    uint32_t offset = macro
                     + ((micro & ~0xFu) << 1)
                     + (micro & 0xFu)
                     + ((y & 8u) << (3 + log_bpp))
                     + ((y & 1u) << 4);
    return ((((offset & ~0x1FFu) << 3)
             + ((offset & 0x1C0u) << 2)
             + (offset & 0x3Fu)
             + ((y & 16u) << 7)
             + (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >> log_bpp);
}

/* Untile one mip level: Xenos tiled order -> linear row-major.
 * `raw` must already be byte-swapped (see swap_elems). out must be blocks_wide*blocks_tall*
 * block_bytes bytes. */
static void unswizzle_mip(const unsigned char *raw, size_t raw_len,
                           uint32_t blocks_wide, uint32_t blocks_tall,
                           int block_bytes, unsigned char *out) {
    memset(out, 0, (size_t)blocks_wide * blocks_tall * (size_t)block_bytes);
    for (uint32_t cby = 0; cby < blocks_tall; cby++) {
        for (uint32_t cbx = 0; cbx < blocks_wide; cbx++) {
            uint64_t src = (uint64_t)xg_address_2d_tiled_offset(cbx, cby, blocks_wide, block_bytes)
                           * (uint64_t)block_bytes;
            uint64_t dst = ((uint64_t)cby * blocks_wide + cbx) * (uint64_t)block_bytes;
            if (src + (uint64_t)block_bytes <= raw_len)
                memcpy(out + dst, raw + src, (size_t)block_bytes);
        }
    }
}

/* Untile a slot whose addressable space is aligned_blocks x aligned_blocks
 * (the 32-block-minimum-tile case), then crop to the real_bw x real_bh
 * region of actual content in its top-left corner. out must be
 * real_bw*real_bh*block_bytes bytes. */
static void unswizzle_mip_region(const unsigned char *raw, size_t raw_len,
                                  uint32_t aligned_bw, uint32_t aligned_bh,
                                  uint32_t real_bw, uint32_t real_bh,
                                  int block_bytes, unsigned char *out) {
    size_t untiled_len = (size_t)aligned_bw * aligned_bh * (size_t)block_bytes;
    unsigned char *untiled = (unsigned char *)malloc(untiled_len > 0 ? untiled_len : 1);
    unswizzle_mip(raw, raw_len, aligned_bw, aligned_bh, block_bytes, untiled);

    memset(out, 0, (size_t)real_bw * real_bh * (size_t)block_bytes);
    for (uint32_t by = 0; by < real_bh; by++) {
        size_t src_off = (size_t)by * aligned_bw * (size_t)block_bytes;
        size_t dst_off = (size_t)by * real_bw * (size_t)block_bytes;
        size_t row_bytes = (size_t)real_bw * (size_t)block_bytes;
        if (src_off + row_bytes <= untiled_len)
            memcpy(out + dst_off, untiled + src_off, row_bytes);
    }
    free(untiled);
}

/* --------------------------------------------------------------------
 * .thb parsing -- ported from parse_thb(). NOTE this is a completely
 * different layout from PS3's: entries are variable-size (this
 * entry's tbb byte count is the difference between consecutive
 * tbb_offsets, not stored directly, except for the last entry).
 * ------------------------------------------------------------------ */

typedef enum { FMT_UNSUPPORTED = 0, FMT_DXT1, FMT_DXT5, FMT_CTX1, FMT_ARGB8 } TexFormat;

typedef struct {
    int index;
    uint32_t meta_offset;
    uint32_t tbb_offset;
    uint32_t tbb_size;
    int raw_fmt_code;   /* 0x52 DXT1, 0x54 DXT5, 0x7C CTX1, 0x86 ARGB8, other = unknown */
    TexFormat fmt;
    uint32_t width;
    uint32_t height;
    int has_stream;
} TextureEntry;

static const char *fmt_name(int raw_fmt_code) {
    switch (raw_fmt_code) {
        case 0x52: return "DXT1";
        case 0x54: return "DXT5";
        case 0x7C: return "CTX1";
        case 0x86: return "ARGB8";
        default: return "UNKNOWN";
    }
}

static int parse_thb(const unsigned char *thb_data, size_t thb_size,
                      TextureEntry **out_entries, int *out_count,
                      char **out_error_message) {
    if (thb_size < 4) {
        *out_error_message = strdup_fmt("error: .thb file is too small to contain a header.");
        return -1;
    }

    uint32_t count = read_be32(thb_data, 0);
    if (count < 1 || count > 64) {
        *out_error_message = strdup_fmt(
            "error: doesn't look like a valid X360-format .thb file -- header "
            "claims %u textures, which is not a plausible value.", count);
        return -1;
    }

    size_t header_area_size = 4 + (size_t)count * 12;
    if (header_area_size > thb_size) {
        *out_error_message = strdup_fmt(
            "error: doesn't look like a valid X360-format .thb file -- ran "
            "out of bytes while reading the %u-entry header table.", count);
        return -1;
    }

    /* Raw (meta_offset, tbb_offset, tbb_size_last_only) triplets first,
     * since a middle entry's real tbb_size depends on the NEXT entry's
     * tbb_offset. */
    uint32_t *f1 = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *f2 = (uint32_t *)malloc(count * sizeof(uint32_t));
    uint32_t *f3 = (uint32_t *)malloc(count * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; i++) {
        size_t base = 4 + (size_t)i * 12;
        f1[i] = read_be32(thb_data, base + 0);
        f2[i] = read_be32(thb_data, base + 4);
        f3[i] = read_be32(thb_data, base + 8);
    }

    TextureEntry *entries = (TextureEntry *)calloc(count, sizeof(TextureEntry));
    if (!entries) {
        free(f1); free(f2); free(f3);
        *out_error_message = strdup_fmt("error: out of memory.");
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        TextureEntry *e = &entries[i];
        e->index = (int)i;
        e->meta_offset = f1[i];
        e->tbb_offset = f2[i];
        e->tbb_size = (i < count - 1) ? (f2[i + 1] - f2[i]) : f3[i];

        /* metadata block: 14 BE u32 fields, 56 bytes total */
        if ((size_t)e->meta_offset + 56 > thb_size) {
            free(f1); free(f2); free(f3); free(entries);
            *out_error_message = strdup_fmt(
                "error: doesn't look like a valid X360-format .thb file -- "
                "ran out of bytes reading metadata for texture %u.", i);
            return -1;
        }
        int raw_fmt = (int)read_be32(thb_data, e->meta_offset + 8 * 4);
        uint32_t packed_dims = read_be32(thb_data, e->meta_offset + 9 * 4);
        uint32_t has_stream_field = read_be32(thb_data, e->meta_offset + 13 * 4);

        e->raw_fmt_code = raw_fmt;
        e->fmt = (raw_fmt == 0x52) ? FMT_DXT1 : (raw_fmt == 0x54) ? FMT_DXT5 :
                 (raw_fmt == 0x7C) ? FMT_CTX1 : (raw_fmt == 0x86) ? FMT_ARGB8 : FMT_UNSUPPORTED;
        e->width = (packed_dims & 0x1FFF) + 1;
        e->height = ((packed_dims >> 13) & 0x1FFF) + 1;
        e->has_stream = has_stream_field != 0;
    }

    free(f1); free(f2); free(f3);
    *out_entries = entries;
    *out_count = (int)count;
    return 0;
}

/* --------------------------------------------------------------------
 * DDS header builders -- ported from _build_block_dds_header()/
 * build_dxt1_dds_header()/build_dxt5_dds_header()/
 * build_cubemap_dds_header(). NOTE: deliberately different from PS3's
 * header -- only mip0 is extracted in this build, so mip_count is
 * always 1 and DDSD_MIPMAPCOUNT is NOT set, matching the Python exactly.
 * ------------------------------------------------------------------ */

static void build_block_dds_header(unsigned char header[128], uint32_t w, uint32_t h,
                                    TexFormat fmt, int is_cubemap) {
    memset(header, 0, 128);
    const uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4,
                   DDSD_PIXELFORMAT = 0x1000, DDSD_LINEARSIZE = 0x80000;
    const uint32_t DDPF_FOURCC = 0x4;
    const uint32_t DDSCAPS_TEXTURE = 0x1000, DDSCAPS_COMPLEX = 0x8;
    const uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00;

    uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
    int block_size = (fmt == FMT_DXT1) ? 8 : 16;
    uint32_t bw = (w + 3) / 4; if (bw < 1) bw = 1;
    uint32_t bh = (h + 3) / 4; if (bh < 1) bh = 1;
    uint32_t linear_size = bw * bh * (uint32_t)block_size;
    uint32_t caps = DDSCAPS_TEXTURE | (is_cubemap ? DDSCAPS_COMPLEX : 0);
    uint32_t caps2 = is_cubemap ? (DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES) : 0;

    memcpy(header, "DDS ", 4);
    write_le32(header, 4, 124);
    write_le32(header, 8, flags);
    write_le32(header, 12, h);
    write_le32(header, 16, w);
    write_le32(header, 20, linear_size);
    write_le32(header, 28, 1); /* mip_count = 1, mip0 only in this build */
    write_le32(header, 76, 32);
    write_le32(header, 80, DDPF_FOURCC);
    memcpy(header + 84, (fmt == FMT_DXT1) ? "DXT1" : "DXT5", 4);
    write_le32(header, 108, caps);
    write_le32(header, 112, caps2);
}

static int write_dds_file(const char *out_path, const unsigned char *payload, size_t payload_len,
                           uint32_t w, uint32_t h, TexFormat fmt, int is_cubemap) {
    unsigned char header[128];
    build_block_dds_header(header, w, h, fmt, is_cubemap);

    FILE *f = fopen(out_path, "wb");
    if (!f)
        return -1;
    size_t written = fwrite(header, 1, 128, f);
    if (payload_len > 0)
        written += fwrite(payload, 1, payload_len, f);
    fclose(f);
    return (written == 128 + payload_len) ? 0 : -1;
}

/* --------------------------------------------------------------------
 * --fix-gamma support: DXT1/DXT5 block decode to RGBA, the X360 gamma
 * LUT, and an uncompressed-RGBA DDS header/writer. Ported from
 * decode_dxt_to_rgba()/_decode_dxt1_block()/X360_GAMMA_LUT/
 * apply_x360_gamma_fix()/build_rgba_dds_header(). Verified byte-exact
 * against the real Python decoder across 20+ random test blocks before
 * being wired in here.
 * ------------------------------------------------------------------ */

static void rgb565_to_rgb888(uint16_t c, unsigned char *r, unsigned char *g, unsigned char *b) {
    *r = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
    *g = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
    *b = (unsigned char)((c & 0x1F) * 255 / 31);
}

static void decode_dxt1_block_rgb(const unsigned char *data, size_t offset,
                                   unsigned char out_rgb[16][3]) {
    uint16_t c0 = (uint16_t)(data[offset] | (data[offset + 1] << 8));
    uint16_t c1 = (uint16_t)(data[offset + 2] | (data[offset + 3] << 8));
    uint32_t idx = (uint32_t)data[offset + 4] | ((uint32_t)data[offset + 5] << 8) |
                   ((uint32_t)data[offset + 6] << 16) | ((uint32_t)data[offset + 7] << 24);

    unsigned char col0[3], col1[3];
    rgb565_to_rgb888(c0, &col0[0], &col0[1], &col0[2]);
    rgb565_to_rgb888(c1, &col1[0], &col1[1], &col1[2]);

    unsigned char colors[4][3];
    memcpy(colors[0], col0, 3);
    memcpy(colors[1], col1, 3);
    if (c0 > c1) {
        for (int k = 0; k < 3; k++) colors[2][k] = (unsigned char)((2 * col0[k] + col1[k]) / 3);
        for (int k = 0; k < 3; k++) colors[3][k] = (unsigned char)((col0[k] + 2 * col1[k]) / 3);
    } else {
        for (int k = 0; k < 3; k++) colors[2][k] = (unsigned char)((col0[k] + col1[k]) / 2);
        colors[3][0] = colors[3][1] = colors[3][2] = 0;
    }
    for (int i = 0; i < 16; i++) {
        int sel = (idx >> (2 * i)) & 0x3;
        memcpy(out_rgb[i], colors[sel], 3);
    }
}

/* Decodes a full untiled DXT1/DXT5 image to a flat RGBA byte buffer
 * (width*height*4 bytes, row-major). `data` must already be untiled
 * linear block data (i.e. what unswizzle_mip()/unswizzle_mip_region()
 * produce). */
static unsigned char *decode_dxt_to_rgba(const unsigned char *data, uint32_t width, uint32_t height,
                                          TexFormat fmt) {
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    unsigned char *pixels = (unsigned char *)malloc((size_t)width * height * 4);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        pixels[i * 4 + 0] = 0; pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0; pixels[i * 4 + 3] = 255;
    }
    int block_size = (fmt == FMT_DXT5) ? 16 : 8;

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t off = ((size_t)by * bw + bx) * (size_t)block_size;
            unsigned char rgb[16][3];
            unsigned char alpha[16];

            if (fmt == FMT_DXT5) {
                unsigned char a0 = data[off], a1 = data[off + 1];
                uint64_t alpha_bits = 0;
                for (int k = 0; k < 6; k++) alpha_bits |= ((uint64_t)data[off + 2 + k]) << (8 * k);
                unsigned char alphas[8];
                alphas[0] = a0; alphas[1] = a1;
                if (a0 > a1) {
                    for (int i = 1; i < 7; i++)
                        alphas[i + 1] = (unsigned char)(((7 - i) * a0 + i * a1) / 7);
                } else {
                    for (int i = 1; i < 5; i++)
                        alphas[i + 1] = (unsigned char)(((5 - i) * a0 + i * a1) / 5);
                    alphas[6] = 0; alphas[7] = 255;
                }
                for (int i = 0; i < 16; i++)
                    alpha[i] = alphas[(alpha_bits >> (3 * i)) & 0x7];
                decode_dxt1_block_rgb(data, off + 8, rgb);
            } else {
                decode_dxt1_block_rgb(data, off, rgb);
                for (int i = 0; i < 16; i++) alpha[i] = 255;
            }

            for (int row = 0; row < 4; row++) {
                for (int col = 0; col < 4; col++) {
                    uint32_t px = bx * 4 + col, py = by * 4 + row;
                    if (px < width && py < height) {
                        size_t pidx = ((size_t)py * width + px) * 4;
                        int bi = row * 4 + col;
                        pixels[pidx + 0] = rgb[bi][0];
                        pixels[pidx + 1] = rgb[bi][1];
                        pixels[pidx + 2] = rgb[bi][2];
                        pixels[pidx + 3] = alpha[bi];
                    }
                }
            }
        }
    }
    return pixels;
}

/* Empirically-fit correction for the X360 hardware's gamma/contrast
 * curve -- see the long comment in x360_thb_tbb_extractv5.py for how
 * this was derived (matched against a real arcade-vs-X360 texture
 * pair). Applied to R/G/B only; alpha is untouched. */
static const unsigned char X360_GAMMA_LUT[256] = {
    3, 4, 6, 9, 14, 15, 17, 18, 23, 23, 25, 27, 28, 29, 31, 31, 35, 34, 34, 37,
    38, 39, 39, 40, 43, 42, 43, 45, 47, 47, 47, 48, 50, 49, 50, 52, 52, 53, 54,
    55, 56, 56, 56, 58, 58, 59, 59, 60, 61, 61, 62, 63, 63, 64, 65, 65, 66, 67,
    67, 68, 69, 69, 70, 71, 71, 72, 72, 74, 75, 75, 77, 78, 78, 79, 80, 81, 82,
    83, 84, 85, 86, 87, 88, 88, 89, 90, 91, 92, 93, 93, 95, 95, 96, 97, 98, 99,
    100, 100, 103, 104, 105, 106, 107, 108, 110, 110, 112, 114, 115, 115, 116,
    118, 120, 120, 121, 122, 124, 124, 126, 127, 128, 129, 130, 131, 133, 133,
    134, 136, 136, 137, 137, 140, 140, 141, 142, 143, 144, 145, 145, 147, 147,
    148, 148, 150, 151, 153, 153, 154, 154, 155, 157, 157, 157, 159, 160, 161,
    162, 162, 163, 164, 164, 165, 166, 167, 167, 168, 169, 171, 171, 171, 172,
    173, 173, 173, 174, 176, 177, 177, 177, 179, 179, 180, 180, 181, 181, 182,
    183, 184, 185, 186, 186, 188, 190, 190, 190, 193, 193, 195, 194, 196, 198,
    199, 200, 201, 203, 205, 205, 206, 208, 209, 210, 211, 213, 214, 215, 215,
    216, 218, 219, 220, 221, 222, 222, 222, 226, 228, 228, 228, 229, 231, 230,
    231, 233, 235, 236, 237, 238, 238, 238, 238, 238, 238, 238, 238, 238, 238,
    238, 238, 238, 238, 238, 238, 238, 238, 238, 238,
};

static void apply_x360_gamma_fix(unsigned char *pixels, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; i++) {
        pixels[i * 4 + 0] = X360_GAMMA_LUT[pixels[i * 4 + 0]];
        pixels[i * 4 + 1] = X360_GAMMA_LUT[pixels[i * 4 + 1]];
        pixels[i * 4 + 2] = X360_GAMMA_LUT[pixels[i * 4 + 2]];
        /* alpha (index 3) left untouched */
    }
}

/* Ported from build_rgba_dds_header(): 128-byte header for uncompressed
 * 32-bit RGBA, 1 mip. Used for gamma-corrected output, since gamma
 * correction needs real decoded pixels, not compressed DXT blocks. */
static void build_rgba_dds_header(unsigned char header[128], uint32_t w, uint32_t h) {
    memset(header, 0, 128);
    const uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4,
                   DDSD_PIXELFORMAT = 0x1000, DDSD_PITCH = 0x8;
    const uint32_t DDPF_RGB_ALPHA = 0x41; /* DDPF_RGB | DDPF_ALPHAPIXELS */
    const uint32_t DDSCAPS_TEXTURE = 0x1000;

    memcpy(header, "DDS ", 4);
    write_le32(header, 4, 124);
    write_le32(header, 8, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH);
    write_le32(header, 12, h);
    write_le32(header, 16, w);
    write_le32(header, 20, w * 4); /* pitch */
    write_le32(header, 28, 1);     /* mip_count */
    write_le32(header, 76, 32);
    write_le32(header, 80, DDPF_RGB_ALPHA);
    write_le32(header, 88, 32);          /* RGBBitCount */
    write_le32(header, 92, 0x000000FF);  /* R mask */
    write_le32(header, 96, 0x0000FF00);  /* G mask */
    write_le32(header, 100, 0x00FF0000); /* B mask */
    write_le32(header, 104, 0xFF000000); /* A mask */
    write_le32(header, 108, DDSCAPS_TEXTURE);
}

static int write_rgba_dds_file(const char *out_path, const unsigned char *rgba, uint32_t w, uint32_t h) {
    unsigned char header[128];
    build_rgba_dds_header(header, w, h);

    FILE *f = fopen(out_path, "wb");
    if (!f)
        return -1;
    size_t payload_len = (size_t)w * h * 4;
    size_t written = fwrite(header, 1, 128, f);
    written += fwrite(rgba, 1, payload_len, f);
    fclose(f);
    return (written == 128 + payload_len) ? 0 : -1;
}

/* --------------------------------------------------------------------
 * CTX1 support: block decoder + uncompressed-RGBA output (this format
 * has no PC DDS FourCC of its own). Ported from _decode_ctx1_block()/
 * render_ctx1_to_rgba_bytes()/build_cubemap_rgba_dds_header(). CTX1
 * stores green+alpha (detail/normal-style data), not colour -- R and B
 * are always written as 0, and it is NEVER gamma-corrected (the X360
 * colour gamma curve doesn't apply to this kind of data).
 * ------------------------------------------------------------------ */

static void decode_ctx1_block_rgba(const unsigned char *block, unsigned char out[16][4]) {
    unsigned char a0 = block[0], g0 = block[1];
    unsigned char a1 = block[2], g1 = block[3];
    uint32_t ci_raw = (uint32_t)block[4] | ((uint32_t)block[5] << 8) |
                       ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);
    uint16_t e0 = (uint16_t)((a0 << 8) | g0);
    uint16_t e1 = (uint16_t)((a1 << 8) | g1);

    unsigned char pal_a[4], pal_g[4];
    pal_a[0] = a0; pal_g[0] = g0;
    pal_a[1] = a1; pal_g[1] = g1;
    if (e0 > e1) {
        pal_a[2] = (unsigned char)((2 * a0 + a1 + 1) / 3);
        pal_g[2] = (unsigned char)((2 * g0 + g1 + 1) / 3);
        pal_a[3] = (unsigned char)((a0 + 2 * a1 + 1) / 3);
        pal_g[3] = (unsigned char)((g0 + 2 * g1 + 1) / 3);
    } else {
        pal_a[2] = (unsigned char)((a0 + a1 + 1) / 2);
        pal_g[2] = (unsigned char)((g0 + g1 + 1) / 2);
        pal_a[3] = 0; pal_g[3] = 0;
    }

    for (int i = 0; i < 16; i++) {
        int sel = (ci_raw >> (2 * i)) & 0x3;
        out[i][0] = 0;
        out[i][1] = pal_g[sel];
        out[i][2] = 0;
        out[i][3] = pal_a[sel];
    }
}

/* Decodes a full untiled CTX1 image (linear 8-byte blocks) to a flat
 * RGBA byte buffer (width*height*4, row-major). */
static unsigned char *render_ctx1_to_rgba(const unsigned char *data, uint32_t width, uint32_t height) {
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    unsigned char *pixels = (unsigned char *)malloc((size_t)width * height * 4);
    memset(pixels, 0, (size_t)width * height * 4);

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t off = ((size_t)by * bw + bx) * 8;
            unsigned char block_px[16][4];
            decode_ctx1_block_rgba(data + off, block_px);
            for (int row = 0; row < 4; row++) {
                for (int col = 0; col < 4; col++) {
                    uint32_t px = bx * 4 + col, py = by * 4 + row;
                    if (px < width && py < height) {
                        size_t pidx = ((size_t)py * width + px) * 4;
                        memcpy(pixels + pidx, block_px[row * 4 + col], 4);
                    }
                }
            }
        }
    }
    return pixels;
}

/* ARGB8 (0x86): uncompressed 32bpp, native byte order per pixel is
 * A,R,G,B (same convention already established for this format on
 * PS3). Uses the SAME Xenos tiling as the block formats -- just with
 * block_bytes=4 and "blocks" being individual pixels (bw=width,
 * bh=height, not divided by 4). Verified against the real
 * ingameicons.thb/.tbb: multiple icons decode to clean, correct
 * images with this exact scheme (the original reference script's
 * author left this format entirely unimplemented -- "KNOWN
 * LIMITATION" -- so this was derived and confirmed from scratch,
 * the same way PS3's swizzled A8R8G8B8 format was). */
static unsigned char *argb8_to_rgba(const unsigned char *argb, uint32_t w, uint32_t h) {
    unsigned char *out = (unsigned char *)malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned char a = argb[i*4+0], r = argb[i*4+1], g = argb[i*4+2], b = argb[i*4+3];
        out[i*4+0] = r; out[i*4+1] = g; out[i*4+2] = b; out[i*4+3] = a;
    }
    return out;
}
static unsigned char *rgba_to_argb8(const unsigned char *rgba, uint32_t w, uint32_t h) {
    unsigned char *out = (unsigned char *)malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned char r = rgba[i*4+0], g = rgba[i*4+1], b = rgba[i*4+2], a = rgba[i*4+3];
        out[i*4+0] = a; out[i*4+1] = r; out[i*4+2] = g; out[i*4+3] = b;
    }
    return out;
}

/* Ported from build_cubemap_rgba_dds_header(): same as
 * build_rgba_dds_header() but flagged as a 6-face cubemap. */
static void build_cubemap_rgba_dds_header(unsigned char header[128], uint32_t w, uint32_t h) {
    build_rgba_dds_header(header, w, h);
    const uint32_t DDSCAPS_TEXTURE = 0x1000, DDSCAPS_COMPLEX = 0x8;
    const uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00;
    write_le32(header, 108, DDSCAPS_TEXTURE | DDSCAPS_COMPLEX);
    write_le32(header, 112, DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES);
}


/* --------------------------------------------------------------------
 * Cubemap mip0 decode -- ported from decode_cubemap_mip0(). Every mip
 * level (including mip0) sits in its own 32-block-aligned slot, laid
 * out mip-major, so mip0 for all 6 faces comes first.
 * ------------------------------------------------------------------ */

/* faces_out must have room for up to 6 buffers of real_bw*real_bh*
 * block_bytes bytes each (caller allocates/frees). Returns the actual
 * number of faces found (may be < 6 for a truncated/partial cubemap). */
static int decode_cubemap_mip0(const unsigned char *tbb_data, size_t tbb_size,
                                uint32_t tbb_offset, uint32_t bw, uint32_t bh, int block_bytes,
                                unsigned char *faces_out[6]) {
    uint32_t aligned_bw = (bw + 31u) & ~31u;
    uint32_t aligned_bh = (bh + 31u) & ~31u;
    uint64_t slot = (uint64_t)aligned_bw * aligned_bh * (uint64_t)block_bytes;

    int n_faces = 6;
    uint64_t remaining = (tbb_offset < tbb_size) ? (tbb_size - tbb_offset) : 0;
    if (remaining < (uint64_t)n_faces * slot) {
        uint64_t fit = slot > 0 ? remaining / slot : 0;
        n_faces = (int)(fit < 1 ? 1 : fit);
    }

    for (int i = 0; i < n_faces; i++) {
        uint64_t off = (uint64_t)tbb_offset + (uint64_t)i * slot;
        size_t avail = (off < tbb_size) ? (size_t)((uint64_t)tbb_size - off) : 0;
        size_t take = (avail < slot) ? avail : (size_t)slot;

        unsigned char *raw_slot = (unsigned char *)malloc(slot > 0 ? (size_t)slot : 1);
        memset(raw_slot, 0, (size_t)slot);
        if (take > 0)
            memcpy(raw_slot, tbb_data + off, take);
        swap_elems(raw_slot, (size_t)slot, block_bytes);

        faces_out[i] = (unsigned char *)malloc((size_t)bw * bh * (size_t)block_bytes);
        unswizzle_mip_region(raw_slot, (size_t)slot, aligned_bw, aligned_bh, bw, bh, block_bytes, faces_out[i]);
        free(raw_slot);
    }
    return n_faces;
}

/* --------------------------------------------------------------------
 * Public entry point. Ported from unpack(), DXT1/DXT5-only subset.
 * ------------------------------------------------------------------ */

int x360_texpack_unpack(const char *thb_path, const char *tbb_path,
                         const char *out_dir, const char *prefix_in,
                         const char *tszip_path, const char *stream_dir_in,
                         int fix_gamma,
                         char **out_log, char **out_error_message) {
    LogBuf log;
    log_init(&log);
    *out_error_message = NULL;

    unsigned char *thb_data = NULL;
    size_t thb_size = 0;
    if (read_whole_file(thb_path, &thb_data, &thb_size) != 0) {
        *out_error_message = strdup_fmt("error: could not read '%s'.", thb_path);
        *out_log = log.data;
        return -1;
    }

    unsigned char *tbb_data = NULL;
    size_t tbb_size = 0;
    if (read_whole_file(tbb_path, &tbb_data, &tbb_size) != 0) {
        free(thb_data);
        *out_error_message = strdup_fmt("error: could not read '%s'.", tbb_path);
        *out_log = log.data;
        return -1;
    }

    TextureEntry *entries = NULL;
    int count = 0;
    if (parse_thb(thb_data, thb_size, &entries, &count, out_error_message) != 0) {
        free(thb_data);
        free(tbb_data);
        *out_log = log.data;
        return -1;
    }

    mkpath(out_dir);

    char prefix[256];
    if (prefix_in)
        snprintf(prefix, sizeof(prefix), "%s", prefix_in);
    else
        derive_prefix(thb_path, prefix, sizeof(prefix));

    char stream_dir[1024];
    if (stream_dir_in)
        snprintf(stream_dir, sizeof(stream_dir), "%s", stream_dir_in);
    else
        derive_dir(tbb_path, stream_dir, sizeof(stream_dir));

    log_append(&log, "%s: %d textures", prefix, count);

    for (int i = 0; i < count; i++) {
        TextureEntry *e = &entries[i];

        if (e->fmt == FMT_UNSUPPORTED) {
            log_append(&log, "  tex%d: format %s not yet handled by this build -- skipping",
                       e->index, fmt_name(e->raw_fmt_code));
            continue;
        }
        if ((uint64_t)e->tbb_offset > (uint64_t)tbb_size) {
            log_append(&log, "  tex%d: WARNING data offset out of bounds -- skipped", e->index);
            continue;
        }

        int block_bytes = (e->fmt == FMT_DXT5) ? 16 : (e->fmt == FMT_ARGB8) ? 4 : 8; /* DXT1 and CTX1 both use 8 */
        uint32_t bw = (e->fmt == FMT_ARGB8) ? e->width : e->width / 4;
        uint32_t bh = (e->fmt == FMT_ARGB8) ? e->height : e->height / 4;
        size_t mip0_size = (size_t)bw * bh * (size_t)block_bytes;

        uint32_t aligned_bw = (bw + 31u) & ~31u;
        uint32_t aligned_bh = (bh + 31u) & ~31u;
        uint64_t slot = (uint64_t)aligned_bw * aligned_bh * (uint64_t)block_bytes;

        char out_path[1200];
        snprintf(out_path, sizeof(out_path), "%s/%s_%d.dds", out_dir, prefix, e->index);

        /* Cubemap case: no stream, tbb holds >= 6 full slots. */
        if (!e->has_stream && (uint64_t)e->tbb_size >= 6ull * slot) {
            unsigned char *faces[6] = {0};
            int n_faces = decode_cubemap_mip0(tbb_data, tbb_size, e->tbb_offset,
                                               bw, bh, block_bytes, faces);

            if (e->fmt == FMT_CTX1 || e->fmt == FMT_ARGB8) {
                size_t rgba_face_size = (size_t)e->width * e->height * 4;
                unsigned char *combined_rgba = (unsigned char *)malloc(rgba_face_size * (size_t)n_faces);
                for (int f = 0; f < n_faces; f++) {
                    unsigned char *face_rgba = (e->fmt == FMT_CTX1)
                        ? render_ctx1_to_rgba(faces[f], e->width, e->height)
                        : argb8_to_rgba(faces[f], e->width, e->height);
                    memcpy(combined_rgba + (size_t)f * rgba_face_size, face_rgba, rgba_face_size);
                    free(face_rgba);
                    free(faces[f]);
                }
                unsigned char cheader[128];
                build_cubemap_rgba_dds_header(cheader, e->width, e->height);
                FILE *cf = fopen(out_path, "wb");
                int cwrite_ok = 0;
                if (cf) {
                    size_t cwritten = fwrite(cheader, 1, 128, cf);
                    cwritten += fwrite(combined_rgba, 1, rgba_face_size * (size_t)n_faces, cf);
                    fclose(cf);
                    cwrite_ok = (cwritten == 128 + rgba_face_size * (size_t)n_faces);
                }
                if (cwrite_ok) {
                    log_append(&log, "  tex%d: %s %ux%u CUBEMAP (%d faces, RGBA) -> %s",
                               e->index, fmt_name(e->raw_fmt_code), e->width, e->height, n_faces, out_path);
                } else {
                    log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
                }
                free(combined_rgba);
                continue;
            }

            unsigned char *combined = (unsigned char *)malloc(mip0_size * (size_t)n_faces);
            for (int f = 0; f < n_faces; f++) {
                memcpy(combined + (size_t)f * mip0_size, faces[f], mip0_size);
                free(faces[f]);
            }
            if (write_dds_file(out_path, combined, mip0_size * (size_t)n_faces,
                                e->width, e->height, e->fmt, 1) == 0) {
                log_append(&log, "  tex%d: %s %ux%u CUBEMAP (%d faces) -> %s",
                           e->index, fmt_name(e->raw_fmt_code), e->width, e->height,
                           n_faces, out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            free(combined);
            continue;
        }

        unsigned char *untiled = (unsigned char *)malloc(mip0_size > 0 ? mip0_size : 1);

        if (e->has_stream) {
            char stream_name[512];
            snprintf(stream_name, sizeof(stream_name), "%s_%d.tstream", prefix, e->index);

            unsigned char *stream_data = NULL;
            size_t stream_size = 0;
            int got_stream = 0;

            if (tszip_path) {
                char *tszip_err = NULL;
                char *found_name = NULL;
                if (find_stream_by_suffix(tszip_path, e->index, &stream_data, &stream_size,
                                           &found_name, &tszip_err) == 0) {
                    got_stream = 1;
                    free(found_name);
                } else {
                    log_append(&log, "  tex%d: WARNING could not read a stream for this texture "
                               "from '%s' (%s) -- skipping (matches the real reference tool's "
                               "behavior: mip0 isn't recoverable without the .tstream)",
                               e->index, tszip_path, tszip_err ? tszip_err : "unknown error");
                    free(tszip_err);
                }
            } else {
                char stream_path[2048];
                snprintf(stream_path, sizeof(stream_path), "%s/%s", stream_dir, stream_name);
                if (read_whole_file(stream_path, &stream_data, &stream_size) == 0) {
                    got_stream = 1;
                } else {
                    log_append(&log, "  tex%d: WARNING has_stream=true but %s not found -- "
                               "skipping (matches the real reference tool's behavior: mip0 "
                               "isn't recoverable without the .tstream)", e->index, stream_path);
                }
            }

            if (!got_stream) {
                free(untiled);
                continue;
            }

            if (stream_size != mip0_size) {
                log_append(&log, "  tex%d: WARNING .tstream is %lu bytes, expected %lu for "
                           "%ux%u %s; using as-is", e->index, (unsigned long)stream_size,
                           (unsigned long)mip0_size, e->width, e->height, fmt_name(e->raw_fmt_code));
            }
            swap_elems(stream_data, stream_size, block_bytes);
            unswizzle_mip(stream_data, stream_size, bw, bh, block_bytes, untiled);
            free(stream_data);
        } else {
            /* Genuine tbb-only entry: mip0 sits in its own 32-block-
             * minimum-tile slot, same pattern as the cubemap case.
             * Each dimension is aligned independently -- NOT forced
             * square. (The original reference script's own version of
             * this used a single square "aligned" value based on
             * width alone, working only because every DXT/CTX1 texture
             * it was tested against happened to have bw==bh; ARGB8
             * icons are frequently non-square -- e.g. 60x69 -- and
             * that assumption corrupts/misaligns the edges for those.) */
            uint32_t aligned_w = aligned_bw > 32 ? aligned_bw : 32;
            uint32_t aligned_h = aligned_bh > 32 ? aligned_bh : 32;
            if (aligned_w < bw) aligned_w = bw;
            if (aligned_h < bh) aligned_h = bh;
            size_t slot_size = (size_t)aligned_w * aligned_h * (size_t)block_bytes;

            unsigned char *chunk = (unsigned char *)malloc(slot_size > 0 ? slot_size : 1);
            memset(chunk, 0, slot_size);
            size_t avail = ((uint64_t)e->tbb_offset < tbb_size)
                           ? (size_t)(tbb_size - e->tbb_offset) : 0;
            size_t take = avail < e->tbb_size ? avail : e->tbb_size;
            if (take > slot_size) take = slot_size;
            if (take > 0)
                memcpy(chunk, tbb_data + e->tbb_offset, take);

            swap_elems(chunk, slot_size, block_bytes);
            unswizzle_mip_region(chunk, slot_size, aligned_w, aligned_h, bw, bh, block_bytes, untiled);
            free(chunk);
        }

        if (e->fmt == FMT_ARGB8) {
            /* Uncompressed -- no gamma-fix distinction needed, and no
             * PC DDS FourCC either (same as CTX1 in that respect). */
            unsigned char *rgba = argb8_to_rgba(untiled, e->width, e->height);
            if (write_rgba_dds_file(out_path, rgba, e->width, e->height) == 0) {
                log_append(&log, "  tex%d: ARGB8 %ux%u -> %s", e->index, e->width, e->height, out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            free(rgba);
        } else if (e->fmt == FMT_CTX1) {
            /* CTX1 has no PC DDS FourCC and is NEVER gamma-corrected --
             * it encodes green+alpha detail data, not colour. */
            unsigned char *rgba = render_ctx1_to_rgba(untiled, e->width, e->height);
            if (write_rgba_dds_file(out_path, rgba, e->width, e->height) == 0) {
                log_append(&log, "  tex%d: CTX1 %ux%u -> %s", e->index, e->width, e->height, out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            free(rgba);
        } else if (fix_gamma) {
            unsigned char *rgba = decode_dxt_to_rgba(untiled, e->width, e->height, e->fmt);
            apply_x360_gamma_fix(rgba, (size_t)e->width * e->height);
            if (write_rgba_dds_file(out_path, rgba, e->width, e->height) == 0) {
                log_append(&log, "  tex%d: %s %ux%u (gamma-corrected) -> %s", e->index,
                           fmt_name(e->raw_fmt_code), e->width, e->height, out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            free(rgba);
        } else if (write_dds_file(out_path, untiled, mip0_size, e->width, e->height, e->fmt, 0) == 0) {
            log_append(&log, "  tex%d: %s %ux%u -> %s", e->index,
                       fmt_name(e->raw_fmt_code), e->width, e->height, out_path);
        } else {
            log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
        }
        free(untiled);
    }

    free(entries);
    free(thb_data);
    free(tbb_data);

    *out_log = log.data;
    return 0;
}

void x360_texpack_free(char *s) {
    free(s);
}

/* ======================================================================
 * REPACK
 * ====================================================================== */

typedef struct { unsigned char r, g, b, a; } Px;

/* Ported from reswizzle_mip(): the inverse of unswizzle_mip(). Tiles one
 * mip level (linear row-major blocks) into Xenos tiled storage, then
 * byte-swaps it (block formats only -- see swap_elems) -- exactly what a
 * real .tstream/.tbb entry looks like on
 * disk. out must be ((blocks_wide+31)&~31)*((blocks_tall+31)&~31)*
 * block_bytes bytes (the function's own internal alignment). */
static void reswizzle_mip(const unsigned char *linear, uint32_t blocks_wide, uint32_t blocks_tall,
                           int block_bytes, unsigned char *out) {
    uint32_t aligned_wide = (blocks_wide + 31u) & ~31u;
    uint32_t aligned_tall = (blocks_tall + 31u) & ~31u;
    size_t out_len = (size_t)aligned_wide * aligned_tall * (size_t)block_bytes;
    memset(out, 0, out_len);
    for (uint32_t cby = 0; cby < blocks_tall; cby++) {
        for (uint32_t cbx = 0; cbx < blocks_wide; cbx++) {
            uint32_t slot = xg_address_2d_tiled_offset(cbx, cby, blocks_wide, block_bytes);
            size_t dst_off = (size_t)slot * (size_t)block_bytes;
            size_t src_off = ((size_t)cby * blocks_wide + cbx) * (size_t)block_bytes;
            memcpy(out + dst_off, linear + src_off, (size_t)block_bytes);
        }
    }
    swap_elems(out, out_len, block_bytes);
}

/* --------------------------------------------------------------------
 * Block encoders -- ported from _encode_dxt1_block()/_encode_dxt5_block()/
 * _encode_ctx1_block(). Min/max-endpoint style (the real encoder this
 * script uses, not a full PCA/cluster-fit one) -- verified byte-exact
 * against the Python reference across 90 random test blocks (30 each)
 * before being wired in here.
 * ------------------------------------------------------------------ */

static uint16_t to565(unsigned char r, unsigned char g, unsigned char b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static void from565(uint16_t v, unsigned char *r, unsigned char *g, unsigned char *b) {
    *r = (unsigned char)(((v >> 11) & 31) * 8);
    *g = (unsigned char)(((v >> 5) & 63) * 4);
    *b = (unsigned char)((v & 31) * 8);
}

static void encode_dxt1_block(const Px block[16], unsigned char out[8]) {
    int has_transparent = 0;
    for (int i = 0; i < 16; i++) {
        if (block[i].a < 128) { has_transparent = 1; break; }
    }

    unsigned char rmax = 0, gmax = 0, bmax = 0, rmin = 255, gmin = 255, bmin = 255;
    for (int i = 0; i < 16; i++) {
        if (block[i].r > rmax) rmax = block[i].r;
        if (block[i].r < rmin) rmin = block[i].r;
        if (block[i].g > gmax) gmax = block[i].g;
        if (block[i].g < gmin) gmin = block[i].g;
        if (block[i].b > bmax) bmax = block[i].b;
        if (block[i].b < bmin) bmin = block[i].b;
    }
    uint16_t c0 = to565(rmax, gmax, bmax);
    uint16_t c1 = to565(rmin, gmin, bmin);

    if (has_transparent) {
        if (c0 > c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    } else {
        if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
        if (c0 == c1) c0 = (uint16_t)(c0 < 0xFFFF ? c0 + 1 : c0);
    }

    unsigned char p0[3], p1[3];
    from565(c0, &p0[0], &p0[1], &p0[2]);
    from565(c1, &p1[0], &p1[1], &p1[2]);

    unsigned char pal[4][3];
    int n_valid;
    memcpy(pal[0], p0, 3);
    memcpy(pal[1], p1, 3);
    if (!has_transparent) {
        for (int k = 0; k < 3; k++) pal[2][k] = (unsigned char)((2 * p0[k] + p1[k]) / 3);
        for (int k = 0; k < 3; k++) pal[3][k] = (unsigned char)((p0[k] + 2 * p1[k]) / 3);
        n_valid = 4;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (unsigned char)((p0[k] + p1[k]) / 2);
        n_valid = 3;
    }

    uint32_t indices = 0;
    for (int i = 0; i < 16; i++) {
        int idx;
        if (has_transparent && block[i].a < 128) {
            idx = 3;
        } else {
            int best = 0;
            long best_d = 1L << 30;
            for (int j = 0; j < n_valid; j++) {
                long dr = (long)block[i].r - pal[j][0];
                long dg = (long)block[i].g - pal[j][1];
                long db = (long)block[i].b - pal[j][2];
                long d = dr * dr + dg * dg + db * db;
                if (d < best_d) { best_d = d; best = j; }
            }
            idx = best;
        }
        int row = i / 4, col = i % 4;
        indices |= ((uint32_t)idx) << (row * 8 + col * 2);
    }

    out[0] = (unsigned char)(c0 & 0xFF); out[1] = (unsigned char)(c0 >> 8);
    out[2] = (unsigned char)(c1 & 0xFF); out[3] = (unsigned char)(c1 >> 8);
    out[4] = (unsigned char)(indices & 0xFF);
    out[5] = (unsigned char)((indices >> 8) & 0xFF);
    out[6] = (unsigned char)((indices >> 16) & 0xFF);
    out[7] = (unsigned char)((indices >> 24) & 0xFF);
}

static void encode_dxt5_block(const Px block[16], unsigned char out[16]) {
    unsigned char a0 = 0, a1 = 255;
    for (int i = 0; i < 16; i++) {
        if (block[i].a > a0) a0 = block[i].a;
        if (block[i].a < a1) a1 = block[i].a;
    }
    if (a0 == a1) {
        if (a0 > 0) a1 = (unsigned char)(a0 - 1);
        else a0 = 1;
    }
    long alpha_pal[8];
    alpha_pal[0] = a0; alpha_pal[1] = a1;
    alpha_pal[2] = (6 * (long)a0 + a1 + 3) / 7;
    alpha_pal[3] = (5 * (long)a0 + 2 * a1 + 3) / 7;
    alpha_pal[4] = (4 * (long)a0 + 3 * a1 + 3) / 7;
    alpha_pal[5] = (3 * (long)a0 + 4 * a1 + 3) / 7;
    alpha_pal[6] = (2 * (long)a0 + 5 * a1 + 3) / 7;
    alpha_pal[7] = ((long)a0 + 6 * a1 + 3) / 7;

    uint64_t alpha_bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        long best_d = 1L << 30;
        for (int j = 0; j < 8; j++) {
            long d = (long)block[i].a - alpha_pal[j];
            d = d * d;
            if (d < best_d) { best_d = d; best = j; }
        }
        alpha_bits |= ((uint64_t)best) << (i * 3);
    }

    unsigned char rmax = 0, gmax = 0, bmax = 0, rmin = 255, gmin = 255, bmin = 255;
    for (int i = 0; i < 16; i++) {
        if (block[i].r > rmax) rmax = block[i].r;
        if (block[i].r < rmin) rmin = block[i].r;
        if (block[i].g > gmax) gmax = block[i].g;
        if (block[i].g < gmin) gmin = block[i].g;
        if (block[i].b > bmax) bmax = block[i].b;
        if (block[i].b < bmin) bmin = block[i].b;
    }
    uint16_t c0 = to565(rmax, gmax, bmax);
    uint16_t c1 = to565(rmin, gmin, bmin);
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (c0 == c1) c0 = (uint16_t)(c0 < 0xFFFF ? c0 + 1 : c0);

    unsigned char p0[3], p1[3];
    from565(c0, &p0[0], &p0[1], &p0[2]);
    from565(c1, &p1[0], &p1[1], &p1[2]);
    unsigned char pal[4][3];
    memcpy(pal[0], p0, 3);
    memcpy(pal[1], p1, 3);
    for (int k = 0; k < 3; k++) pal[2][k] = (unsigned char)((2 * p0[k] + p1[k]) / 3);
    for (int k = 0; k < 3; k++) pal[3][k] = (unsigned char)((p0[k] + 2 * p1[k]) / 3);

    uint32_t colour_idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        long best_d = 1L << 30;
        for (int j = 0; j < 4; j++) {
            long dr = (long)block[i].r - pal[j][0];
            long dg = (long)block[i].g - pal[j][1];
            long db = (long)block[i].b - pal[j][2];
            long d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = j; }
        }
        int row = i / 4, col = i % 4;
        colour_idx |= ((uint32_t)best) << (row * 8 + col * 2);
    }

    out[0] = a0; out[1] = a1;
    for (int k = 0; k < 6; k++) out[2 + k] = (unsigned char)((alpha_bits >> (8 * k)) & 0xFF);
    out[8] = (unsigned char)(c0 & 0xFF); out[9] = (unsigned char)(c0 >> 8);
    out[10] = (unsigned char)(c1 & 0xFF); out[11] = (unsigned char)(c1 >> 8);
    out[12] = (unsigned char)(colour_idx & 0xFF);
    out[13] = (unsigned char)((colour_idx >> 8) & 0xFF);
    out[14] = (unsigned char)((colour_idx >> 16) & 0xFF);
    out[15] = (unsigned char)((colour_idx >> 24) & 0xFF);
}

static void encode_ctx1_block(const Px block[16], unsigned char out[8]) {
    int ag_a[16], ag_g[16];
    for (int i = 0; i < 16; i++) { ag_a[i] = block[i].a; ag_g[i] = block[i].g; }

    int max_k = -1, min_k = 1 << 30, max_i = 0, min_i = 0;
    for (int i = 0; i < 16; i++) {
        int key = (ag_a[i] << 8) | ag_g[i];
        if (key > max_k) { max_k = key; max_i = i; }
        if (key < min_k) { min_k = key; min_i = i; }
    }
    unsigned char a0 = (unsigned char)ag_a[max_i], g0 = (unsigned char)ag_g[max_i];
    unsigned char a1 = (unsigned char)ag_a[min_i], g1 = (unsigned char)ag_g[min_i];
    int e0 = (a0 << 8) | g0, e1 = (a1 << 8) | g1;

    int pal_a[4], pal_g[4];
    pal_a[0] = a0; pal_g[0] = g0; pal_a[1] = a1; pal_g[1] = g1;
    if (e0 > e1) {
        pal_a[2] = (2 * a0 + a1 + 1) / 3; pal_g[2] = (2 * g0 + g1 + 1) / 3;
        pal_a[3] = (a0 + 2 * a1 + 1) / 3; pal_g[3] = (g0 + 2 * g1 + 1) / 3;
    } else {
        pal_a[2] = (a0 + a1 + 1) / 2; pal_g[2] = (g0 + g1 + 1) / 2;
        pal_a[3] = 0; pal_g[3] = 0;
    }

    uint32_t indices = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        long best_d = 1L << 30;
        for (int j = 0; j < 4; j++) {
            long da = ag_a[i] - pal_a[j], dg = ag_g[i] - pal_g[j];
            long d = da * da + dg * dg;
            if (d < best_d) { best_d = d; best = j; }
        }
        int row = i / 4, col = i % 4;
        indices |= ((uint32_t)best) << (row * 8 + col * 2);
    }

    out[0] = a0; out[1] = g0; out[2] = a1; out[3] = g1;
    out[4] = (unsigned char)(indices & 0xFF);
    out[5] = (unsigned char)((indices >> 8) & 0xFF);
    out[6] = (unsigned char)((indices >> 16) & 0xFF);
    out[7] = (unsigned char)((indices >> 24) & 0xFF);
}

/* --------------------------------------------------------------------
 * Mip chain generation. Deliberately uses a box filter (simple 2x2
 * average) for downsampling instead of the Python reference's PIL
 * LANCZOS -- a common, defensible choice for mipmap generation, and it
 * only affects the smaller/lower mip levels, never mip0 (the texture
 * people actually look at up close).
 * ------------------------------------------------------------------ */

static unsigned char *box_downsample(const unsigned char *src, uint32_t sw, uint32_t sh,
                                      uint32_t dw, uint32_t dh) {
    unsigned char *dst = (unsigned char *)malloc((size_t)dw * dh * 4);
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;

            unsigned long sums[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (uint32_t sy = y0; sy < y1; sy++) {
                for (uint32_t sx = x0; sx < x1; sx++) {
                    size_t idx = ((size_t)sy * sw + sx) * 4;
                    for (int k = 0; k < 4; k++) sums[k] += src[idx + k];
                    n++;
                }
            }
            size_t didx = ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++)
                dst[didx + k] = (unsigned char)(sums[k] / (n > 0 ? n : 1));
        }
    }
    return dst;
}

typedef struct {
    unsigned char *blocks; /* encoded, malloc'd */
    size_t size;
    uint32_t bw, bh; /* dimensions in 4x4 blocks */
} MipLevel;

/* Ported from build_mip_chain(): cascading box-filter halving down to a
 * 4x4-pixel (1-block) floor, encoding each level. Returns the number of
 * levels; caller frees each level's .blocks and the returned array. */
static int build_mip_chain(const unsigned char *base_rgba, uint32_t width, uint32_t height,
                            TexFormat fmt, MipLevel **out_levels) {
    uint32_t w = width, h = height;
    int count = 1;
    while (w > 4 || h > 4) {
        w = (w / 2 > 4) ? w / 2 : 4;
        h = (h / 2 > 4) ? h / 2 : 4;
        count++;
    }

    MipLevel *levels = (MipLevel *)calloc((size_t)count, sizeof(MipLevel));
    unsigned char *cur_rgba = (unsigned char *)malloc((size_t)width * height * 4);
    memcpy(cur_rgba, base_rgba, (size_t)width * height * 4);
    uint32_t cw = width, ch = height;
    size_t block_size = (fmt == FMT_DXT5) ? 16 : 8;

    for (int level = 0; level < count; level++) {
        uint32_t bw = cw / 4, bh = ch / 4;
        unsigned char *blocks = (unsigned char *)malloc((size_t)bw * bh * block_size);

        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                Px block[16];
                for (int row = 0; row < 4; row++) {
                    for (int col = 0; col < 4; col++) {
                        uint32_t px = bx * 4 + col, py = by * 4 + row;
                        size_t idx = ((size_t)py * cw + px) * 4;
                        int bi = row * 4 + col;
                        block[bi].r = cur_rgba[idx]; block[bi].g = cur_rgba[idx + 1];
                        block[bi].b = cur_rgba[idx + 2]; block[bi].a = cur_rgba[idx + 3];
                    }
                }
                size_t off = ((size_t)by * bw + bx) * block_size;
                if (fmt == FMT_DXT1) encode_dxt1_block(block, blocks + off);
                else if (fmt == FMT_DXT5) encode_dxt5_block(block, blocks + off);
                else encode_ctx1_block(block, blocks + off);
            }
        }

        levels[level].blocks = blocks;
        levels[level].size = (size_t)bw * bh * block_size;
        levels[level].bw = bw;
        levels[level].bh = bh;

        if (level + 1 < count) {
            uint32_t nw = (cw / 2 > 4) ? cw / 2 : 4;
            uint32_t nh = (ch / 2 > 4) ? ch / 2 : 4;
            unsigned char *next_rgba = box_downsample(cur_rgba, cw, ch, nw, nh);
            free(cur_rgba);
            cur_rgba = next_rgba;
            cw = nw; ch = nh;
        }
    }
    free(cur_rgba);
    *out_levels = levels;
    return count;
}

/* --------------------------------------------------------------------
 * Replacement image loading -- DDS only in this build (DXT1/DXT5/
 * uncompressed RGBA). PNG input is not yet supported (needs a full PNG
 * decoder). Ported from load_replacement_image()'s DDS branch.
 * ------------------------------------------------------------------ */

static int load_replacement_dds(const char *path, TexFormat target_fmt,
                                 unsigned char **out_rgba, uint32_t *out_w, uint32_t *out_h,
                                 unsigned char **out_raw, size_t *out_raw_len,
                                 char **out_error) {
    unsigned char *data = NULL;
    size_t size = 0;
    if (read_whole_file(path, &data, &size) != 0) {
        *out_error = strdup_fmt("error: could not read '%s'.", path);
        return -1;
    }
    if (size < 128 || memcmp(data, "DDS ", 4) != 0) {
        free(data);
        *out_error = strdup_fmt("error: '%s' is not a valid DDS file.", path);
        return -1;
    }
    uint32_t h = read_le32(data, 12);
    uint32_t w = read_le32(data, 16);
    uint32_t pf_flags = read_le32(data, 80);
    *out_raw = NULL;
    *out_raw_len = 0;

    if (pf_flags & 0x4) {
        int is_dxt1 = memcmp(data + 84, "DXT1", 4) == 0;
        int is_dxt5 = memcmp(data + 84, "DXT5", 4) == 0;
        if (!is_dxt1 && !is_dxt5) {
            free(data);
            *out_error = strdup_fmt(
                "error: '%s' uses an unsupported DDS FourCC -- use DXT1, DXT5, "
                "or uncompressed RGBA.", path);
            return -1;
        }
        int block_bytes = is_dxt1 ? 8 : 16;
        size_t payload_len = (size_t)(w / 4) * (h / 4) * (size_t)block_bytes;
        if (128 + payload_len > size) {
            free(data);
            *out_error = strdup_fmt("error: '%s' texture data is truncated.", path);
            return -1;
        }
        TexFormat src_fmt = is_dxt1 ? FMT_DXT1 : FMT_DXT5;
        unsigned char *rgba = decode_dxt_to_rgba(data + 128, w, h, src_fmt);
        if (src_fmt == target_fmt) {
            *out_raw = (unsigned char *)malloc(payload_len);
            memcpy(*out_raw, data + 128, payload_len);
            *out_raw_len = payload_len;
        }
        *out_rgba = rgba; *out_w = w; *out_h = h;
        free(data);
        return 0;
    }

    /* Uncompressed RGBA DDS -- same byte layout as our internal RGBA
     * representation, so it's a direct copy. */
    size_t expected = (size_t)w * h * 4;
    if (128 + expected > size) {
        free(data);
        *out_error = strdup_fmt(
            "error: '%s' declares %ux%u uncompressed RGBA but doesn't have "
            "enough pixel data.", path, w, h);
        return -1;
    }
    unsigned char *rgba = (unsigned char *)malloc(expected);
    memcpy(rgba, data + 128, expected);
    *out_rgba = rgba; *out_w = w; *out_h = h;
    free(data);
    return 0;
}

static int find_replacement_dds_path(const char *input_dir, const char *prefix, int index,
                                      char *out_path, size_t out_path_size) {
    snprintf(out_path, out_path_size, "%s/%s_%d.dds", input_dir, prefix, index);
    FILE *f = fopen(out_path, "rb");
    if (!f)
        return -1;
    fclose(f);
    return 0;
}

#define X360_PAGE_SIZE 4096u
static uint64_t page_align(uint64_t n) {
    return ((n + X360_PAGE_SIZE - 1) / X360_PAGE_SIZE) * X360_PAGE_SIZE;
}

/* Ported from patch_tbb_tstream_entry()/patch_tbb_only_entry() -- they're
 * identical except for the starting level (1 to skip mip0, which lives
 * in the .tstream instead; 0 for tbb-only entries with no stream). */
static void patch_tbb_entry(const MipLevel *levels, int level_count, int start_level,
                             unsigned char *tbb, size_t tbb_size,
                             uint32_t tbb_offset, uint32_t tbb_entry_size, int block_bytes) {
    uint64_t cursor = tbb_offset;
    for (int level = start_level; level < level_count; level++) {
        uint32_t aligned_w = (levels[level].bw + 31u) & ~31u;
        uint32_t aligned_h = (levels[level].bh + 31u) & ~31u;
        size_t tiled_len = (size_t)aligned_w * aligned_h * (size_t)block_bytes;
        unsigned char *tiled = (unsigned char *)malloc(tiled_len);
        reswizzle_mip(levels[level].blocks, levels[level].bw, levels[level].bh, block_bytes, tiled);

        uint64_t end = cursor + tiled_len;
        if (end > (uint64_t)tbb_offset + tbb_entry_size || end > tbb_size) {
            free(tiled);
            break;
        }
        memcpy(tbb + cursor, tiled, tiled_len);
        free(tiled);
        cursor = page_align(end);
    }
}

/* --------------------------------------------------------------------
 * Public entry point. Ported from repack(). Covers flat (non-cubemap)
 * DXT1/DXT5/CTX1 textures, both .tstream and tbb-only. NOT yet covered:
 * cubemap repack, PNG replacement input.
 * ------------------------------------------------------------------ */

int x360_texpack_repack(const char *thb_path, const char *tbb_path,
                         const char *input_dir, const char *out_dir,
                         const char *prefix_in, const char *orig_tszip_path,
                         char **out_log, char **out_error_message) {
    LogBuf log;
    log_init(&log);
    *out_error_message = NULL;

    unsigned char *thb_data = NULL;
    size_t thb_size = 0;
    if (read_whole_file(thb_path, &thb_data, &thb_size) != 0) {
        *out_error_message = strdup_fmt("error: could not read '%s'.", thb_path);
        *out_log = log.data;
        return -1;
    }

    unsigned char *orig_tbb = NULL;
    size_t tbb_size = 0;
    if (read_whole_file(tbb_path, &orig_tbb, &tbb_size) != 0) {
        free(thb_data);
        *out_error_message = strdup_fmt("error: could not read '%s'.", tbb_path);
        *out_log = log.data;
        return -1;
    }

    TextureEntry *entries = NULL;
    int count = 0;
    if (parse_thb(thb_data, thb_size, &entries, &count, out_error_message) != 0) {
        free(thb_data);
        free(orig_tbb);
        *out_log = log.data;
        return -1;
    }

    mkpath(out_dir);

    char prefix[256];
    if (prefix_in)
        snprintf(prefix, sizeof(prefix), "%s", prefix_in);
    else
        derive_prefix(thb_path, prefix, sizeof(prefix));

    unsigned char *tbb = (unsigned char *)malloc(tbb_size > 0 ? tbb_size : 1);
    memcpy(tbb, orig_tbb, tbb_size);

    /* Streamed textures' mip0 data gets collected here and packaged
     * into one <prefix>.tszip at the end, instead of loose .tstream
     * files. */
    TszipEntry *tszip_entries = (TszipEntry *)calloc((size_t)count, sizeof(TszipEntry));
    int tszip_entry_count = 0;

    log_append(&log, "%s: %d textures", prefix, count);

    for (int i = 0; i < count; i++) {
        TextureEntry *e = &entries[i];

        if (e->fmt == FMT_UNSUPPORTED) {
            log_append(&log, "  tex%d: format %s not supported for repack -- skipping",
                       e->index, fmt_name(e->raw_fmt_code));
            continue;
        }

        int block_bytes = (e->fmt == FMT_DXT5) ? 16 : (e->fmt == FMT_ARGB8) ? 4 : 8;
        uint32_t bw0 = (e->fmt == FMT_ARGB8) ? e->width : e->width / 4;
        uint32_t bh0 = (e->fmt == FMT_ARGB8) ? e->height : e->height / 4;
        uint32_t aligned_bw = (bw0 + 31u) & ~31u;
        uint32_t aligned_bh = (bh0 + 31u) & ~31u;
        uint64_t slot = (uint64_t)aligned_bw * aligned_bh * (uint64_t)block_bytes;

        if (!e->has_stream && (uint64_t)e->tbb_size >= 6ull * slot) {
            log_append(&log, "  tex%d: cubemap -- repack not supported yet, skipping", e->index);
            continue;
        }

        char repl_path[1200];
        if (find_replacement_dds_path(input_dir, prefix, e->index, repl_path, sizeof(repl_path)) != 0) {
            /* Unedited -- the tbb copy already has this texture's
             * unchanged data (nothing to patch), but a streamed
             * texture's real mip0 lives ONLY in the .tszip/.tstream,
             * not the tbb. Without carrying it forward here, the
             * repacked output would silently lose it even though
             * nothing was actually edited. */
            if (e->has_stream) {
                char stream_name[512];
                snprintf(stream_name, sizeof(stream_name), "%s_%d.tstream", prefix, e->index);
                unsigned char *sdata = NULL; size_t ssize = 0;
                char *actual_name = NULL;

                if (orig_tszip_path) {
                    char *tszip_err = NULL;
                    if (find_stream_by_suffix(orig_tszip_path, e->index, &sdata, &ssize,
                                               &actual_name, &tszip_err) != 0) {
                        log_append(&log, "  tex%d: WARNING could not read this texture's stream "
                                   "from '%s' (%s)", e->index, orig_tszip_path,
                                   tszip_err ? tszip_err : "unknown error");
                        free(tszip_err);
                    }
                }
                if (!sdata) {
                    char stream_path[2048];
                    snprintf(stream_path, sizeof(stream_path), "%s/%s", input_dir, stream_name);
                    if (read_whole_file(stream_path, &sdata, &ssize) != 0) {
                        char tbb_dir[1024];
                        derive_dir(tbb_path, tbb_dir, sizeof(tbb_dir));
                        snprintf(stream_path, sizeof(stream_path), "%s/%s", tbb_dir, stream_name);
                        read_whole_file(stream_path, &sdata, &ssize);
                    }
                }
                if (sdata) {
                    TszipEntry *te = &tszip_entries[tszip_entry_count++];
                    snprintf(te->name, sizeof(te->name), "%s", actual_name ? actual_name : stream_name);
                    te->data = sdata;
                    te->size = ssize;
                    log_append(&log, "  tex%d: no replacement found -- unchanged (stream carried "
                               "forward)", e->index);
                } else {
                    log_append(&log, "  tex%d: WARNING no replacement, and no stream found (no "
                               "original .tszip given, and no loose .tstream file) -- this "
                               "texture's mip0 data will be lost", e->index);
                }
                free(actual_name);
            } else {
                log_append(&log, "  tex%d: no replacement found -- unchanged", e->index);
            }
            continue;
        }

        unsigned char *rgba = NULL;
        uint32_t rw = 0, rh = 0;
        unsigned char *raw_passthrough = NULL;
        size_t raw_len = 0;
        char *load_err = NULL;
        if (load_replacement_dds(repl_path, e->fmt, &rgba, &rw, &rh,
                                  &raw_passthrough, &raw_len, &load_err) != 0) {
            log_append(&log, "  tex%d: WARNING could not load replacement '%s' (%s) -- unchanged",
                       e->index, repl_path, load_err ? load_err : "unknown error");
            free(load_err);
            continue;
        }

        if (rw != e->width || rh != e->height) {
            unsigned char *resized = box_downsample(rgba, rw, rh, e->width, e->height);
            free(rgba);
            rgba = resized;
            log_append(&log, "  tex%d: replacement was %ux%u, resized to %ux%u",
                       e->index, rw, rh, e->width, e->height);
            free(raw_passthrough);
            raw_passthrough = NULL;
            raw_len = 0;
        }

        MipLevel *levels = NULL;
        int level_count;
        if (e->fmt == FMT_ARGB8) {
            /* Uncompressed, single level (this codebase's whole X360
             * model only ever deals with mip0) -- skip build_mip_chain
             * entirely, since it's hardwired for 4x4-block compressed
             * formats. Just convert to native byte order directly. */
            level_count = 1;
            levels = (MipLevel *)calloc(1, sizeof(MipLevel));
            levels[0].blocks = rgba_to_argb8(rgba, e->width, e->height);
            levels[0].size = (size_t)e->width * e->height * 4;
            levels[0].bw = e->width;
            levels[0].bh = e->height;
        } else {
            level_count = build_mip_chain(rgba, e->width, e->height, e->fmt, &levels);
        }
        free(rgba);

        if (e->has_stream) {
            const unsigned char *mip0_data = levels[0].blocks;
            if (raw_passthrough)
                mip0_data = raw_passthrough;

            uint32_t ts_aligned_w = (bw0 + 31u) & ~31u;
            uint32_t ts_aligned_h = (bh0 + 31u) & ~31u;
            size_t ts_len = (size_t)ts_aligned_w * ts_aligned_h * (size_t)block_bytes;
            unsigned char *ts_tiled = (unsigned char *)malloc(ts_len);
            reswizzle_mip(mip0_data, bw0, bh0, block_bytes, ts_tiled);

            TszipEntry *te = &tszip_entries[tszip_entry_count++];
            snprintf(te->name, sizeof(te->name), "%s_%d.tstream", prefix, e->index);
            te->data = ts_tiled; /* ownership moves to tszip_entries, freed after build_tszip */
            te->size = ts_len;

            patch_tbb_entry(levels, level_count, 1, tbb, tbb_size,
                             e->tbb_offset, e->tbb_size, block_bytes);
            log_append(&log, "  tex%d: REPLACED with %s -> %s (%lu bytes, packaged into %s.tszip) "
                       "+ tbb mips 1..%d", e->index, repl_path, te->name,
                       (unsigned long)ts_len, prefix, level_count - 1);
        } else {
            patch_tbb_entry(levels, level_count, 0, tbb, tbb_size,
                             e->tbb_offset, e->tbb_size, block_bytes);
            log_append(&log, "  tex%d: REPLACED with %s (tbb-only, %d mip levels)",
                       e->index, repl_path, level_count);
        }

        free(raw_passthrough);
        for (int L = 0; L < level_count; L++) free(levels[L].blocks);
        free(levels);
    }

    if (tszip_entry_count > 0) {
        char tszip_path[1200];
        snprintf(tszip_path, sizeof(tszip_path), "%s/%s.tszip", out_dir, prefix);
        if (build_tszip(tszip_entries, tszip_entry_count, tszip_path) == 0)
            log_append(&log, "wrote %s (%d streamed textures)", tszip_path, tszip_entry_count);
        else
            log_append(&log, "WARNING could not write %s", tszip_path);
    }
    for (int i = 0; i < tszip_entry_count; i++)
        free(tszip_entries[i].data);
    free(tszip_entries);

    char out_tbb_path[1200];
    snprintf(out_tbb_path, sizeof(out_tbb_path), "%s/%s.tbb", out_dir, prefix);
    FILE *tf = fopen(out_tbb_path, "wb");
    int tbb_write_ok = 0;
    if (tf) {
        size_t written = fwrite(tbb, 1, tbb_size, tf);
        fclose(tf);
        tbb_write_ok = (written == tbb_size);
    }
    if (tbb_write_ok)
        log_append(&log, "wrote %s (%lu bytes)", out_tbb_path, (unsigned long)tbb_size);
    else
        log_append(&log, "WARNING could not write %s", out_tbb_path);

    char out_thb_path[1200];
    snprintf(out_thb_path, sizeof(out_thb_path), "%s/%s.thb", out_dir, prefix);
    FILE *thf = fopen(out_thb_path, "wb");
    int thb_write_ok = 0;
    if (thf) {
        size_t written = fwrite(thb_data, 1, thb_size, thf);
        fclose(thf);
        thb_write_ok = (written == thb_size);
    }
    if (thb_write_ok)
        log_append(&log, "copied %s (metadata unchanged)", out_thb_path);
    else
        log_append(&log, "WARNING could not write %s", out_thb_path);

    free(entries);
    free(thb_data);
    free(orig_tbb);
    free(tbb);

    *out_log = log.data;
    return 0;
}

