#include "ps3_texpack.h"
#include "tszip_reader.h"
#include "tszip_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#if defined(_WIN32)
#include <direct.h>
#define PS3_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define PS3_MKDIR(path) mkdir(path, 0755)
#endif

/* --------------------------------------------------------------------
 * Small helpers: growable log buffer, formatted string allocation,
 * big-endian reads (the .thb format is big-endian), little-endian
 * writes (DDS headers are little-endian), whole-file reading.
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
    size_t needed = lb->len + line_len + 2; /* +1 newline, +1 nul */
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

/* Portable duplicate-a-string helper (avoids relying on POSIX strdup /
 * MSVC _strdup being available under strict C99). */
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

/* Reads an entire file into a malloc'd buffer. Returns 0 on success. */
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

/* Finds a stream by its exact "_<index>.tstream" suffix.
 * Ignores the preceding filename, since multiple texture variants
 * can share a .tszip and use streams named after the base asset.
 * This matches the approach already verified for the Wii format. */
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
            if (out_name) *out_name = strdup_fmt("%s", found);
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

/* mkdir -p, minimal version (creates one path, ignoring "already exists"). */
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
            PS3_MKDIR(buf); /* ignore errors: "already exists" is fine */
            buf[i] = c;
        }
    }
    PS3_MKDIR(buf);
}

/* Extracts the filename (no directory, no extension) from a path,
 * e.g. "C:/models/mcqueen.thb" -> "mcqueen". */
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

/* Directory containing path, e.g. "C:/model/mcqueen.tbb" -> "C:/models".
 * Returns "." if path has no directory component. */
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
 * Format-specific logic, ported field-for-field from parse_thb()/
 * unpack() in ps3_thb_tbb_extract_v6.py.
 * ------------------------------------------------------------------ */

typedef enum { FMT_NONE = 0, FMT_DXT1, FMT_DXT5, FMT_ARGB8 } TexFormat;

typedef struct {
    int index;
    uint32_t thb_offset;
    uint32_t tbb_offset;
    uint32_t texture_size;
    TexFormat fmt;
    uint32_t width;
    uint32_t height;
    uint32_t mips;      /* Used only for FMT_ARGB8; DXT1/DXT5 always
                     * use a full mip chain. */
    int is_linear;      /* Used only for FMT_ARGB8; indicates linear
                     * storage. If clear, the texture is swizzled.
                     * Swizzling requires power-of-2 dimensions. */
    int has_stream;
} TextureEntry;

static uint32_t swizzle_offset(uint32_t x, uint32_t y, int log2w, int log2h) {
    uint32_t offset = 0;
    int shift = 0;
    while (log2w > 0 || log2h > 0) {
        if (log2w > 0) {
            offset |= (x & 1u) << shift;
            x >>= 1; shift++; log2w--;
        }
        if (log2h > 0) {
            offset |= (y & 1u) << shift;
            y >>= 1; shift++; log2h--;
        }
    }
    return offset;
}

static int is_pow2(uint32_t v) { return v >= 1 && (v & (v - 1)) == 0; }
static int log2_u32(uint32_t v) { int n = 0; while (v > 1) { v >>= 1; n++; } return n; }

/* Converts an ARGB8 mip level between swizzled and linear order.
 * The same permutation works both ways. Byte order is unchanged;
 * swap_argb_bgra() handles that separately anyways. */
static void swizzle_transpose_argb8(const unsigned char *src, unsigned char *dst,
                                     uint32_t w, uint32_t h, int to_linear) {
    if (!is_pow2(w) || !is_pow2(h)) {
        /* Shouldn't happen in practice (hardware requires power-of-2
         * for swizzled addressing), but never index out of bounds if
         * it somehow does -- fall back to a straight copy. */
        memcpy(dst, src, (size_t)w * h * 4);
        return;
    }
    int log2w = log2_u32(w), log2h = log2_u32(h);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t swizzled_idx = swizzle_offset(x, y, log2w, log2h);
            uint32_t linear_idx = y * w + x;
            uint32_t src_idx = to_linear ? swizzled_idx : linear_idx;
            uint32_t dst_idx = to_linear ? linear_idx : swizzled_idx;
            memcpy(dst + (size_t)dst_idx * 4, src + (size_t)src_idx * 4, 4);
        }
    }
}

/* Sum of w*h*4 across `mips` halving levels -- FMT_ARGB8 is
 * uncompressed (no 4x4 block padding needed) and, unlike DXT1/DXT5,
 * doesn't always carry a full chain down to 1x1; the real level count
 * comes from the descriptor's own mips field. */
static uint64_t argb8_chain_size(uint32_t w, uint32_t h, uint32_t mips) {
    uint64_t total = 0;
    if (mips < 1) mips = 1;
    for (uint32_t i = 0; i < mips; i++) {
        total += (uint64_t)w * h * 4;
        w = w / 2; if (w < 1) w = 1;
        h = h / 2; if (h < 1) h = 1;
    }
    return total;
}

/* Ported from full_chain(). Total byte size of a complete mip chain,
 * base resolution down to 1x1. */
static uint64_t full_chain_size(uint32_t w, uint32_t h, int bpp) {
    uint64_t total = 0;
    for (;;) {
        uint32_t bw = (w + 3) / 4; if (bw < 1) bw = 1;
        uint32_t bh = (h + 3) / 4; if (bh < 1) bh = 1;
        total += (uint64_t)bw * (uint64_t)bh * (uint64_t)bpp;
        if (w == 1 && h == 1)
            break;
        w = w / 2; if (w < 1) w = 1;
        h = h / 2; if (h < 1) h = 1;
    }
    return total;
}

/* Ported from build_dds_header()/write_dds(). Writes a standard 128-byte
 * DDS header for DXT1/DXT5/ARGB8 data (optionally a 6-face cubemap).
 * mip_count_override: 0 means "compute by halving down to 1x1" (DXT1/
 * DXT5's existing convention); nonzero uses that count directly
 * (ARGB8, whose real level count comes from the descriptor and isn't
 * always a full chain). */
static void build_dds_header(unsigned char header[128], uint32_t w, uint32_t h,
                              TexFormat fmt, int is_cubemap, int mip_count_override) {
    memset(header, 0, 128);

    const uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4,
                   DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000,
                   DDSD_LINEARSIZE = 0x80000, DDSD_PITCH = 0x8;
    const uint32_t DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_ALPHAPIXELS = 0x1;
    const uint32_t DDSCAPS_COMPLEX = 0x8, DDSCAPS_TEXTURE = 0x1000, DDSCAPS_MIPMAP = 0x400000;
    const uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00;

    int mip_count;
    if (mip_count_override > 0) {
        mip_count = mip_count_override;
    } else {
        mip_count = 1;
        uint32_t tw = w, th = h;
        while (!(tw == 1 && th == 1)) {
            tw = tw / 2; if (tw < 1) tw = 1;
            th = th / 2; if (th < 1) th = 1;
            mip_count++;
        }
    }

    uint32_t caps = DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | (mip_count > 1 ? DDSCAPS_MIPMAP : 0);
    uint32_t caps2 = is_cubemap ? (DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES) : 0;

    memcpy(header, "DDS ", 4);
    write_le32(header, 4, 124);
    write_le32(header, 12, h);
    write_le32(header, 16, w);
    write_le32(header, 28, (uint32_t)mip_count);
    /* bytes 32..75: 44-byte reserved block, already zeroed by memset */
    write_le32(header, 76, 32);

    if (fmt == FMT_ARGB8) {
        uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT |
                          DDSD_MIPMAPCOUNT | DDSD_PITCH;
        write_le32(header, 8, flags);
        write_le32(header, 20, w * 4); /* pitch: bytes per scanline */
        write_le32(header, 80, DDPF_RGB | DDPF_ALPHAPIXELS);
        write_le32(header, 88, 32);         /* bit count */
        write_le32(header, 92, 0xFF0000);   /* R mask */
        write_le32(header, 96, 0xFF00);     /* G mask */
        write_le32(header, 100, 0xFF);      /* B mask */
        write_le32(header, 104, 0xFF000000u); /* A mask */
    } else {
        uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT |
                          DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE;
        write_le32(header, 8, flags);
        int block_size = (fmt == FMT_DXT1) ? 8 : 16;
        uint32_t bw = (w + 3) / 4; if (bw < 1) bw = 1;
        uint32_t bh = (h + 3) / 4; if (bh < 1) bh = 1;
        write_le32(header, 20, bw * bh * (uint32_t)block_size);
        write_le32(header, 80, DDPF_FOURCC);
        memcpy(header + 84, (fmt == FMT_DXT1) ? "DXT1" : "DXT5", 4);
        write_le32(header, 88, 0);
        write_le32(header, 92, 0);
        write_le32(header, 96, 0);
        write_le32(header, 100, 0);
        write_le32(header, 104, 0);
    }

    write_le32(header, 108, caps);
    write_le32(header, 112, caps2);
    write_le32(header, 116, 0);
    write_le32(header, 120, 0);
    write_le32(header, 124, 0);
}

/* Converts one ARGB8 mip chain between native byte order (A,R,G,B per
 * pixel, big-endian-ish as stored in the .tbb) and DDS byte order
 * (B,G,R,A, matching the BGRA convention used throughout this
 * project's other uncompressed-format DDS output). Same permutation
 * both directions -- swapping A<->B leaves G in place and swaps R<->
 * nothing (ARGB -> BGRA is: byte0 A<->byte3, byte1 R<->byte2, i.e.
 * a full 4-byte reverse). Pure byte reordering, no data loss either
 * way. */
static void swap_argb_bgra(unsigned char *data, size_t n_pixels) {
    for (size_t i = 0; i < n_pixels; i++) {
        unsigned char *p = data + i * 4;
        unsigned char a = p[0], r = p[1], g = p[2], b = p[3];
        p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    }
}

static int write_dds_file(const char *out_path, const unsigned char *chunk, size_t chunk_len,
                           uint32_t w, uint32_t h, TexFormat fmt, int is_cubemap) {
    unsigned char header[128];
    build_dds_header(header, w, h, fmt, is_cubemap, 0);

    FILE *f = fopen(out_path, "wb");
    if (!f)
        return -1;
    size_t written = fwrite(header, 1, 128, f);
    if (chunk_len > 0)
        written += fwrite(chunk, 1, chunk_len, f);
    fclose(f);
    return (written == 128 + chunk_len) ? 0 : -1;
}

/* Ported from parse_thb() + validate_ps3_thb(). */
static int parse_thb(const unsigned char *thb_data, size_t thb_size,
                      TextureEntry **out_entries, int *out_count,
                      char **out_error_message) {
    if (thb_size < 4) {
        *out_error_message = strdup_fmt(
            "error: .thb file is too small to contain a header.");
        return -1;
    }

    uint32_t count = read_be32(thb_data, 0);
    if (count < 1 || count > 64) {
        *out_error_message = strdup_fmt(
            "error: doesn't look like a valid PS3-format .thb file -- header "
            "claims %u textures, which is not a plausible value. This is "
            "most likely a Wii .thb file (different header layout).", count);
        return -1;
    }

    size_t header_area_size = 4 + (size_t)count * 12;
    if (header_area_size > thb_size) {
        *out_error_message = strdup_fmt(
            "error: doesn't look like a valid PS3-format .thb file -- ran "
            "out of bytes while reading the %u-entry header table.", count);
        return -1;
    }

    TextureEntry *entries = (TextureEntry *)calloc(count, sizeof(TextureEntry));
    if (!entries) {
        *out_error_message = strdup_fmt("error: out of memory.");
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        size_t base = 4 + (size_t)i * 12;
        entries[i].index = (int)i;
        entries[i].thb_offset = read_be32(thb_data, base + 0);
        entries[i].tbb_offset = read_be32(thb_data, base + 4);
        entries[i].texture_size = read_be32(thb_data, base + 8);
    }

    for (uint32_t i = 0; i < count; i++) {
        TextureEntry *e = &entries[i];
        /* descriptor is 32 bytes: extra_fields, unused, then 6 more u32s
         * (fmt_and_mips, unk1, wh, unk2, unk3, unk4) starting at +8 */
        if ((size_t)e->thb_offset + 32 > thb_size) {
            free(entries);
            *out_error_message = strdup_fmt(
                "error: doesn't look like a valid PS3-format .thb file -- "
                "ran out of bytes while parsing descriptor fields for "
                "texture %u. This is most likely a Wii .thb file (different "
                "header layout).", i);
            return -1;
        }
        uint32_t extra_fields = read_be32(thb_data, e->thb_offset);
        e->has_stream = extra_fields > 1;
        uint32_t fmt_and_mips = read_be32(thb_data, e->thb_offset + 8);
        uint32_t wh = read_be32(thb_data, e->thb_offset + 16);
        uint32_t top_byte = (fmt_and_mips >> 24) & 0xFF;
        /* 0x20 is CELL_GCM_TEXTURE_LN -- set means linear (simple
         * row-major) layout, clear means swizzled (Morton/Z-order
         * tiled, real RSX cache-locality addressing, only valid when
         * both dimensions are power-of-2). DXT1/DXT5 are observed
         * without this bit ever set (0x86/0x88 exactly), so masking
         * it off before comparing doesn't affect them; ARGB8 is seen
         * both ways (0x85 swizzled, 0xA5 linear) depending on whether
         * the texture's dimensions are power-of-2. */
        uint32_t base_fmt = top_byte & ~0x20u;
        if (top_byte == 0x88) e->fmt = FMT_DXT5;
        else if (top_byte == 0x86) e->fmt = FMT_DXT1;
        else if (base_fmt == 0x85) e->fmt = FMT_ARGB8;
        else e->fmt = FMT_NONE;
        e->is_linear = (top_byte & 0x20u) != 0;
        e->mips = (fmt_and_mips >> 16) & 0xFF;
        e->width = (wh >> 16) & 0xFFFF;
        e->height = wh & 0xFFFF;
    }

    int bad = 0;
    for (uint32_t i = 0; i < count; i++) {
        TextureEntry *e = &entries[i];
        /* Not power-of-2 -- some real PS3 textures (icons etc.) use
         * arbitrary dimensions; DXT1/DXT5 block padding handles that
         * fine, so only check plausible bounds here. An unrecognized
         * format byte does NOT belong in this check: a real PS3 file
         * can legitimately contain a texture format this tool (and
         * the reference script it's ported from) doesn't decode --
         * that's a per-texture capability gap with its own graceful
         * fallback (raw-bytes dump) further down, not evidence the
         * whole file is actually some other platform's container. */
        int w_ok = e->width >= 1 && e->width <= 4096;
        int h_ok = e->height >= 1 && e->height <= 4096;
        int bounds_ok = e->thb_offset < thb_size;
        if (!(w_ok && h_ok && bounds_ok))
            bad++;
    }
    if (count > 0 && (double)bad / (double)count > 0.3) {
        free(entries);
        *out_error_message = strdup_fmt(
            "error: doesn't look like a valid PS3-format .thb file -- "
            "%d/%u textures had impossible values (implausible "
            "dimensions and/or out-of-bounds offsets). This is most "
            "likely a Wii .thb file (different, fixed-32-byte "
            "descriptor format).", bad, count);
        return -1;
    }

    *out_entries = entries;
    *out_count = (int)count;
    return 0;
}

/* --------------------------------------------------------------------
 * Public entry point. Ported from unpack().
 * ------------------------------------------------------------------ */

int ps3_texpack_unpack(const char *thb_path, const char *tbb_path,
                        const char *out_dir, const char *prefix_in,
                        const char *tszip_path, const char *stream_dir_in,
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

        if ((uint64_t)e->tbb_offset + (uint64_t)e->texture_size > (uint64_t)tbb_size) {
            log_append(&log, "  tex%d: WARNING data offset/size out of bounds -- skipped", e->index);
            continue;
        }
        const unsigned char *chunk = tbb_data + e->tbb_offset;
        size_t chunk_len = e->texture_size;

        char out_path[1200];

        if (e->fmt == FMT_NONE) {
            snprintf(out_path, sizeof(out_path), "%s/%s_%d.raw", out_dir, prefix, e->index);
            FILE *f = fopen(out_path, "wb");
            if (f) {
                fwrite(chunk, 1, chunk_len, f);
                fclose(f);
                log_append(&log, "  tex%d: unrecognized format byte -- wrote raw bytes to %s",
                           e->index, out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            continue;
        }

        if (e->fmt == FMT_ARGB8) {
            uint64_t decoded_size = argb8_chain_size(e->width, e->height, e->mips);
            if (decoded_size > chunk_len) {
                log_append(&log, "  tex%d: WARNING declared %ux%u mips=%u needs %llu bytes but "
                           "only %zu are present -- wrote raw bytes instead", e->index,
                           e->width, e->height, e->mips, (unsigned long long)decoded_size, chunk_len);
                snprintf(out_path, sizeof(out_path), "%s/%s_%d.raw", out_dir, prefix, e->index);
                FILE *f = fopen(out_path, "wb");
                if (f) { fwrite(chunk, 1, chunk_len, f); fclose(f); }
                continue;
            }
            /* Trailing bytes beyond the decoded pixel data (seen in
             * real files -- not yet understood, possibly packer
             * padding/leftover) aren't shown to the user, but repack
             * preserves them byte-for-byte from the original rather
             * than dropping them. */
            unsigned char *pixels = (unsigned char *)malloc((size_t)decoded_size);
            if (e->is_linear) {
                memcpy(pixels, chunk, (size_t)decoded_size);
            } else {
                /* De-swizzle each mip level independently (its own,
                 * successively-halved dimensions), matching how the
                 * real hardware addresses each level. */
                uint32_t lw = e->width, lh = e->height;
                size_t pos = 0;
                for (uint32_t m = 0; m < (e->mips < 1 ? 1 : e->mips); m++) {
                    size_t lvl_len = (size_t)lw * lh * 4;
                    swizzle_transpose_argb8(chunk + pos, pixels + pos, lw, lh, 1);
                    pos += lvl_len;
                    lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
                }
            }
            swap_argb_bgra(pixels, (size_t)decoded_size / 4);

            unsigned char header[128];
            build_dds_header(header, e->width, e->height, FMT_ARGB8, 0, (int)(e->mips < 1 ? 1 : e->mips));
            snprintf(out_path, sizeof(out_path), "%s/%s_%d.dds", out_dir, prefix, e->index);
            FILE *f = fopen(out_path, "wb");
            if (f) {
                fwrite(header, 1, 128, f);
                fwrite(pixels, 1, (size_t)decoded_size, f);
                fclose(f);
                uint64_t extra = chunk_len - decoded_size;
                log_append(&log, "  tex%d: A8R8G8B8 %ux%u mips=%u%s -> %s", e->index,
                           e->width, e->height, e->mips < 1 ? 1 : e->mips,
                           extra > 0 ? " (+trailing bytes preserved for repack)" : "", out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            free(pixels);
            continue;
        }

        int bpp = (e->fmt == FMT_DXT1) ? 8 : 16;
        uint64_t declared_size = full_chain_size(e->width, e->height, bpp);

        /* Cubemap case: texture_size is (at least) 6x the single-face
         * chain size and it has no stream. A small number of leading
         * bytes (shared/duplicated small mips) come before the 6 faces. */
        if (!e->has_stream && (uint64_t)chunk_len >= declared_size * 6) {
            uint64_t leading = (uint64_t)chunk_len - declared_size * 6;
            snprintf(out_path, sizeof(out_path), "%s/%s_%d.dds", out_dir, prefix, e->index);
            if (write_dds_file(out_path, chunk + leading, (size_t)(declared_size * 6),
                                e->width, e->height, e->fmt, 1) == 0) {
                log_append(&log, "  tex%d: %s %ux%u CUBEMAP (6 faces), %llu leading bytes "
                           "skipped -> %s", e->index, e->fmt == FMT_DXT1 ? "DXT1" : "DXT5",
                           e->width, e->height, (unsigned long long)leading, out_path);
            } else {
                log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
            }
            continue;
        }

        /* Regular case, possibly combined with a .tstream. */
        const unsigned char *data_ptr = chunk;
        size_t data_len = chunk_len;
        unsigned char *combined = NULL;

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
                               "from '%s' (%s)", e->index, tszip_path,
                               tszip_err ? tszip_err : "unknown error");
                    free(tszip_err);
                }
            } else {
                char stream_path[2048];
                snprintf(stream_path, sizeof(stream_path), "%s/%s", stream_dir, stream_name);
                if (read_whole_file(stream_path, &stream_data, &stream_size) == 0)
                    got_stream = 1;
                else
                    log_append(&log, "  tex%d: WARNING has_stream=true but %s not found -- "
                               "using base resolution only (will be visually WRONG)",
                               e->index, stream_path);
            }

            if (got_stream) {
                combined = (unsigned char *)malloc(stream_size + chunk_len);
                memcpy(combined, stream_data, stream_size);
                memcpy(combined + stream_size, chunk, chunk_len);
                free(stream_data);
                data_ptr = combined;
                data_len = stream_size + chunk_len;

                uint64_t expected_total = full_chain_size(e->width, e->height, bpp);
                if ((uint64_t)data_len != expected_total) {
                    log_append(&log, "  tex%d: WARNING combined stream+tbb size (%lu) doesn't "
                               "match the expected full chain for %ux%u (%llu) -- output may "
                               "be visually wrong", e->index, (unsigned long)data_len, e->width, e->height,
                               (unsigned long long)expected_total);
                }
            }
        }

        snprintf(out_path, sizeof(out_path), "%s/%s_%d.dds", out_dir, prefix, e->index);
        if (write_dds_file(out_path, data_ptr, data_len, e->width, e->height, e->fmt, 0) == 0) {
            log_append(&log, "  tex%d: %s %ux%u -> %s", e->index,
                       e->fmt == FMT_DXT1 ? "DXT1" : "DXT5", e->width, e->height, out_path);
        } else {
            log_append(&log, "  tex%d: WARNING could not write %s", e->index, out_path);
        }

        if (combined)
            free(combined);
    }

    free(entries);
    free(thb_data);
    free(tbb_data);

    *out_log = log.data;
    return 0;
}

void ps3_texpack_free(char *s) {
    free(s);
}

/* ======================================================================
 * REPACK
 * ====================================================================== */

static uint32_t read_le32(const unsigned char *data, size_t offset) {
    return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
           ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}

/* Big-endian write, since the .thb format itself is big-endian (unlike
 * DDS headers, which are little-endian -- hence "_local" to distinguish
 * from write_le32 above). */
static void write_be32_local(unsigned char *buf, size_t offset, uint32_t value) {
    buf[offset + 0] = (unsigned char)((value >> 24) & 0xFF);
    buf[offset + 1] = (unsigned char)((value >> 16) & 0xFF);
    buf[offset + 2] = (unsigned char)((value >> 8) & 0xFF);
    buf[offset + 3] = (unsigned char)(value & 0xFF);
}

/* --------------------------------------------------------------------
 * PS3's real encoder: PCA/range-fit initial endpoints, then a few
 * cluster-fit least-squares refinement iterations. Genuinely different
 * from (and higher quality than) X360's simple min/max encoder --
 * opaque-only DXT1 (no punch-through alpha mode). Verified byte-exact
 * against the actual Python script across 60 random test blocks (30
 * DXT1 + 30 DXT5) before being wired in here.
 * ------------------------------------------------------------------ */

typedef struct { double r, g, b; } Vec3d;

static uint16_t rgb_to_565(Vec3d p) {
    double r = p.r, g = p.g, b = p.b;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    long ri = (long)round(r), gi = (long)round(g), bi = (long)round(b);
    uint16_t r5 = (uint16_t)(((ri * 31 + 127) / 255) & 0x1F);
    uint16_t g6 = (uint16_t)(((gi * 63 + 127) / 255) & 0x3F);
    uint16_t b5 = (uint16_t)(((bi * 31 + 127) / 255) & 0x1F);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static Vec3d rgb565_to_rgb_d(uint16_t c) {
    Vec3d v;
    v.r = (double)(((c >> 11) & 0x1F) * 255 / 31);
    v.g = (double)(((c >> 5) & 0x3F) * 255 / 63);
    v.b = (double)((c & 0x1F) * 255 / 31);
    return v;
}

static void dxt1_palette_d(uint16_t c0, uint16_t c1, Vec3d pal[4]) {
    Vec3d a = rgb565_to_rgb_d(c0), b = rgb565_to_rgb_d(c1);
    pal[0] = a;
    pal[1] = b;
    pal[2].r = floor((2*a.r+b.r)/3); pal[2].g = floor((2*a.g+b.g)/3); pal[2].b = floor((2*a.b+b.b)/3);
    pal[3].r = floor((a.r+2*b.r)/3); pal[3].g = floor((a.g+2*b.g)/3); pal[3].b = floor((a.b+2*b.b)/3);
}

static double dist2_d(Vec3d a, Vec3d b) {
    double dr = a.r-b.r, dg = a.g-b.g, db = a.b-b.b;
    return 0.30*dr*dr + 0.59*dg*dg + 0.11*db*db;
}

static void pca_axis(const Vec3d pixels[16], Vec3d *mean_out, Vec3d *axis_out) {
    Vec3d mean = {0,0,0};
    for (int i = 0; i < 16; i++) { mean.r += pixels[i].r; mean.g += pixels[i].g; mean.b += pixels[i].b; }
    mean.r /= 16.0; mean.g /= 16.0; mean.b /= 16.0;

    double cov[3][3] = {{0}};
    for (int i = 0; i < 16; i++) {
        double d[3] = { pixels[i].r-mean.r, pixels[i].g-mean.g, pixels[i].b-mean.b };
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                cov[a][b] += d[a]*d[b];
    }

    double axis[3] = {1.0, 1.0, 1.0};
    for (int iter = 0; iter < 6; iter++) {
        double nxt[3];
        for (int a = 0; a < 3; a++) {
            nxt[a] = 0;
            for (int b = 0; b < 3; b++) nxt[a] += cov[a][b] * axis[b];
        }
        double length = sqrt(nxt[0]*nxt[0] + nxt[1]*nxt[1] + nxt[2]*nxt[2]);
        if (length < 1e-9) break;
        axis[0] = nxt[0]/length; axis[1] = nxt[1]/length; axis[2] = nxt[2]/length;
    }
    *mean_out = mean;
    axis_out->r = axis[0]; axis_out->g = axis[1]; axis_out->b = axis[2];
}

static void encode_dxt1_block_ps3(const Vec3d pixels[16], unsigned char out[8]) {
    Vec3d mean, axis;
    pca_axis(pixels, &mean, &axis);

    double projections[16];
    for (int i = 0; i < 16; i++)
        projections[i] = (pixels[i].r-mean.r)*axis.r + (pixels[i].g-mean.g)*axis.g + (pixels[i].b-mean.b)*axis.b;
    double lo_t = projections[0], hi_t = projections[0];
    for (int i = 1; i < 16; i++) { if (projections[i]<lo_t) lo_t=projections[i]; if (projections[i]>hi_t) hi_t=projections[i]; }

    Vec3d hi = { mean.r+axis.r*hi_t, mean.g+axis.g*hi_t, mean.b+axis.b*hi_t };
    Vec3d lo = { mean.r+axis.r*lo_t, mean.g+axis.g*lo_t, mean.b+axis.b*lo_t };

    uint16_t c0 = rgb_to_565(hi), c1 = rgb_to_565(lo);
    if (c0 < c1) { uint16_t t=c0; c0=c1; c1=t; }
    if (c0 == c1) {
        c0 = (uint16_t)(c0 + 1 <= 0xFFFF ? c0 + 1 : c0);
        if (c0 <= c1) c1 = (uint16_t)(c1 > 0 ? c1 - 1 : 0);
    }

    static const double coeff[4][2] = { {1.0,0.0}, {0.0,1.0}, {2.0/3.0,1.0/3.0}, {1.0/3.0,2.0/3.0} };

    for (int rep = 0; rep < 4; rep++) {
        Vec3d pal[4];
        dxt1_palette_d(c0, c1, pal);
        int indices[16];
        for (int i = 0; i < 16; i++) {
            int best = 0; double best_d = dist2_d(pixels[i], pal[0]);
            for (int k = 1; k < 4; k++) { double d = dist2_d(pixels[i], pal[k]); if (d < best_d) { best_d = d; best = k; } }
            indices[i] = best;
        }

        double s00=0,s01=0,s11=0, rhs0[3]={0,0,0}, rhs1[3]={0,0,0};
        for (int i = 0; i < 16; i++) {
            double a = coeff[indices[i]][0], b = coeff[indices[i]][1];
            s00 += a*a; s01 += a*b; s11 += b*b;
            double p[3] = {pixels[i].r, pixels[i].g, pixels[i].b};
            for (int k = 0; k < 3; k++) { rhs0[k] += a*p[k]; rhs1[k] += b*p[k]; }
        }

        double det = s00*s11 - s01*s01;
        if (fabs(det) > 1e-9) {
            double f0[3], f1[3];
            for (int k = 0; k < 3; k++) {
                f0[k] = (rhs0[k]*s11 - rhs1[k]*s01) / det;
                f1[k] = (rhs1[k]*s00 - rhs0[k]*s01) / det;
            }
            Vec3d fitted0 = {f0[0], f0[1], f0[2]};
            Vec3d fitted1 = {f1[0], f1[1], f1[2]};
            uint16_t nc0 = rgb_to_565(fitted0), nc1 = rgb_to_565(fitted1);
            if (nc0 > nc1) { c0 = nc0; c1 = nc1; }
            else if (nc1 > nc0) { c0 = nc1; c1 = nc0; }
            else {
                c0 = nc0;
                c1 = (uint16_t)(nc1 ? nc1 - 1 : 1);
            }
        } else {
            break;
        }
    }

    Vec3d pal[4];
    dxt1_palette_d(c0, c1, pal);
    uint32_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0; double best_d = dist2_d(pixels[i], pal[0]);
        for (int k = 1; k < 4; k++) { double d = dist2_d(pixels[i], pal[k]); if (d < best_d) { best_d = d; best = k; } }
        bits |= ((uint32_t)best) << (2*i);
    }

    out[0]=(unsigned char)(c0&0xFF); out[1]=(unsigned char)(c0>>8);
    out[2]=(unsigned char)(c1&0xFF); out[3]=(unsigned char)(c1>>8);
    out[4]=(unsigned char)(bits&0xFF); out[5]=(unsigned char)((bits>>8)&0xFF);
    out[6]=(unsigned char)((bits>>16)&0xFF); out[7]=(unsigned char)((bits>>24)&0xFF);
}

static void encode_dxt5_block_ps3(const unsigned char pixels[16][4], unsigned char out[16]) {
    int amin = pixels[0][3], amax = pixels[0][3];
    for (int i = 1; i < 16; i++) { if (pixels[i][3]<amin) amin=pixels[i][3]; if (pixels[i][3]>amax) amax=pixels[i][3]; }

    int cand_a0[2], cand_a1[2], n_cand = 1;
    cand_a0[0] = amax; cand_a1[0] = amin;
    if (amin == 0 || amax == 255) { cand_a0[1] = amin; cand_a1[1] = amax; n_cand = 2; }

    long best_err = -1; int best_a0 = 0, best_a1 = 0; uint64_t best_bits = 0;
    for (int c = 0; c < n_cand; c++) {
        int a0 = cand_a0[c], a1 = cand_a1[c];
        long ramp[8];
        ramp[0] = a0; ramp[1] = a1;
        if (a0 > a1) {
            for (int i = 1; i < 7; i++) ramp[i+1] = ((7-i)*(long)a0 + i*(long)a1) / 7;
        } else {
            for (int i = 1; i < 5; i++) ramp[i+1] = ((5-i)*(long)a0 + i*(long)a1) / 5;
            ramp[6] = 0; ramp[7] = 255;
        }
        uint64_t bits = 0; long err = 0;
        for (int i = 0; i < 16; i++) {
            int a = pixels[i][3];
            int best_idx = 0; long best_d = (a-ramp[0])*(a-ramp[0]);
            for (int k = 1; k < 8; k++) {
                long d = (a-ramp[k])*(a-ramp[k]);
                if (d < best_d) { best_d = d; best_idx = k; }
            }
            err += best_d;
            bits |= ((uint64_t)best_idx) << (3*i);
        }
        if (best_err < 0 || err < best_err) { best_err = err; best_a0 = a0; best_a1 = a1; best_bits = bits; }
    }

    out[0] = (unsigned char)best_a0; out[1] = (unsigned char)best_a1;
    for (int k = 0; k < 6; k++) out[2+k] = (unsigned char)((best_bits >> (8*k)) & 0xFF);

    Vec3d rgb_pixels[16];
    for (int i = 0; i < 16; i++) { rgb_pixels[i].r = pixels[i][0]; rgb_pixels[i].g = pixels[i][1]; rgb_pixels[i].b = pixels[i][2]; }
    unsigned char rgb_block[8];
    encode_dxt1_block_ps3(rgb_pixels, rgb_block);
    memcpy(out+8, rgb_block, 8);
}

/* --------------------------------------------------------------------
 * Mip chain generation (box-filter downsampling, same deliberate
 * simplification as X360's -- only affects lower mip levels, never
 * mip0). Unlike X360, PS3's chain goes all the way to 1x1, so a mip
 * level can be smaller than one 4x4 block; those samples clamp to the
 * available pixels, matching the Python encode_linear()'s clamping.
 * ------------------------------------------------------------------ */

static unsigned char *box_downsample_ps3(const unsigned char *src, uint32_t sw, uint32_t sh,
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
            unsigned long sums[4] = {0,0,0,0};
            uint32_t n = 0;
            for (uint32_t sy = y0; sy < y1; sy++) {
                for (uint32_t sx = x0; sx < x1; sx++) {
                    size_t idx = ((size_t)sy * sw + sx) * 4;
                    for (int k = 0; k < 4; k++) sums[k] += src[idx + k];
                    n++;
                }
            }
            size_t didx = ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++) dst[didx + k] = (unsigned char)(sums[k] / (n > 0 ? n : 1));
        }
    }
    return dst;
}

/* Encodes one linear (non-tiled) mip level from an rgba buffer of size
 * cw x ch (may be smaller than 4x4), clamping out-of-range block
 * samples to the nearest real pixel -- matches encode_linear()'s
 * `min(x, w-1), min(y, h-1)` behavior exactly. */
static void encode_mip_level_ps3(const unsigned char *rgba, uint32_t cw, uint32_t ch,
                                  TexFormat fmt, unsigned char *out) {
    uint32_t bw = (cw + 3) / 4, bh = (ch + 3) / 4;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            unsigned char block[16][4];
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx*4 + (i % 4), y = by*4 + (i / 4);
                if (x > cw - 1) x = cw - 1;
                if (y > ch - 1) y = ch - 1;
                size_t idx = ((size_t)y * cw + x) * 4;
                memcpy(block[i], rgba + idx, 4);
            }
            size_t block_size = (fmt == FMT_DXT5) ? 16 : 8;
            size_t off = ((size_t)by * bw + bx) * block_size;
            if (fmt == FMT_DXT1) {
                Vec3d pixels[16];
                for (int i = 0; i < 16; i++) { pixels[i].r=block[i][0]; pixels[i].g=block[i][1]; pixels[i].b=block[i][2]; }
                encode_dxt1_block_ps3(pixels, out + off);
            } else {
                encode_dxt5_block_ps3(block, out + off);
            }
        }
    }
}

typedef struct {
    unsigned char *data;
    size_t size;
} ChainLevel;

/* Ported from encode_full_chain(): base resolution down to 1x1, base
 * mip first. Returns the full concatenated byte buffer (caller frees). */
static unsigned char *encode_full_chain_ps3(const unsigned char *base_rgba, uint32_t w, uint32_t h,
                                             TexFormat fmt, size_t *out_size) {
    int bpp = (fmt == FMT_DXT5) ? 16 : 8;
    uint64_t total = full_chain_size(w, h, bpp);
    unsigned char *out = (unsigned char *)malloc((size_t)total);

    unsigned char *cur_rgba = (unsigned char *)malloc((size_t)w * h * 4);
    memcpy(cur_rgba, base_rgba, (size_t)w * h * 4);
    uint32_t cw = w, ch = h;
    size_t pos = 0;

    for (;;) {
        uint32_t bw = (cw + 3) / 4, bh = (ch + 3) / 4;
        size_t block_size = (fmt == FMT_DXT5) ? 16 : 8;
        size_t level_size = (size_t)bw * bh * block_size;
        encode_mip_level_ps3(cur_rgba, cw, ch, fmt, out + pos);
        pos += level_size;

        if (cw == 1 && ch == 1)
            break;
        uint32_t nw = cw / 2 > 1 ? cw / 2 : 1;
        uint32_t nh = ch / 2 > 1 ? ch / 2 : 1;
        unsigned char *next_rgba = box_downsample_ps3(cur_rgba, cw, ch, nw, nh);
        free(cur_rgba);
        cur_rgba = next_rgba;
        cw = nw; ch = nh;
    }
    free(cur_rgba);
    *out_size = pos;
    return out;
}

/* --------------------------------------------------------------------
 * Replacement image loading -- DDS only in this build (DXT1/DXT5/
 * uncompressed RGBA). PNG is not yet supported (needs a full PNG
 * decoder) -- unlike the Python reference, which supports both via PIL.
 * No raw-passthrough shortcut, matching the Python reference exactly:
 * every replacement is fully decoded then re-encoded, even when the
 * source format already matches the target.
 * ------------------------------------------------------------------ */

static TexFormat detect_replacement_format(const char *path) {
    unsigned char header[128];
    FILE *f = fopen(path, "rb");
    if (!f) return FMT_NONE;
    size_t n = fread(header, 1, 128, f);
    fclose(f);
    if (n < 128 || memcmp(header, "DDS ", 4) != 0) return FMT_NONE;
    if (memcmp(header + 84, "DXT1", 4) == 0) return FMT_DXT1;
    if (memcmp(header + 84, "DXT5", 4) == 0) return FMT_DXT5;
    uint32_t pff = read_le32(header, 80);
    uint32_t bits = read_le32(header, 88);
    if (!(pff & 0x4) && bits == 32) return FMT_ARGB8;
    return FMT_NONE;
}

/* Reads an uncompressed 32bpp DDS's pixel data (using its own declared
 * mip count and dimensions), converts DDS byte order back to native
 * ARGB, and returns it as a malloc'd buffer. out_mips/out_w/out_h
 * reflect what was actually found in the file (may differ from the
 * slot's original values if the replacement is a different size). */
static unsigned char *read_argb8_dds(const char *path, uint32_t *out_w, uint32_t *out_h,
                                      uint32_t *out_mips, size_t *out_len) {
    unsigned char *data = NULL; size_t size = 0;
    if (read_whole_file(path, &data, &size) != 0) return NULL;
    if (size < 128 || memcmp(data, "DDS ", 4) != 0) { free(data); return NULL; }
    uint32_t flags = read_le32(data, 8);
    uint32_t h = read_le32(data, 12);
    uint32_t w = read_le32(data, 16);
    uint32_t decl_mips = read_le32(data, 28);
    uint32_t mips = ((flags & 0x20000) && decl_mips) ? decl_mips : 1;
    uint64_t total = argb8_chain_size(w, h, mips);
    if (128 + total > size) { free(data); return NULL; }
    unsigned char *pixels = (unsigned char *)malloc((size_t)total);
    memcpy(pixels, data + 128, (size_t)total);
    free(data);
    swap_argb_bgra(pixels, (size_t)total / 4);
    *out_w = w; *out_h = h; *out_mips = mips; *out_len = (size_t)total;
    return pixels;
}

static int load_replacement_dds_ps3(const char *path, unsigned char **out_rgba,
                                     uint32_t *out_w, uint32_t *out_h, char **out_error) {
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
        size_t payload_len = (size_t)((w + 3) / 4) * ((h + 3) / 4) * (size_t)block_bytes;
        if (128 + payload_len > size) {
            free(data);
            *out_error = strdup_fmt("error: '%s' texture data is truncated.", path);
            return -1;
        }
        TexFormat src_fmt = is_dxt1 ? FMT_DXT1 : FMT_DXT5;
        /* decode_linear-equivalent: reuse the existing unpack-side decoder
         * via a tiny local shim, since ps3_texpack.c doesn't already
         * expose one publicly for this purpose. */
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t off = ((size_t)by * bw + bx) * (size_t)block_bytes;
                unsigned char block_rgba[16][4];
                if (src_fmt == FMT_DXT5) {
                    unsigned char a0 = data[128+off], a1 = data[128+off+1];
                    uint64_t alpha_bits = 0;
                    for (int k = 0; k < 6; k++) alpha_bits |= ((uint64_t)data[128+off+2+k]) << (8*k);
                    unsigned char alphas[8]; alphas[0]=a0; alphas[1]=a1;
                    if (a0 > a1) { for (int i=1;i<7;i++) alphas[i+1]=(unsigned char)(((7-i)*a0+i*a1)/7); }
                    else { for (int i=1;i<5;i++) alphas[i+1]=(unsigned char)(((5-i)*a0+i*a1)/5); alphas[6]=0; alphas[7]=255; }
                    uint16_t c0 = (uint16_t)(data[128+off+8] | (data[128+off+9]<<8));
                    uint16_t c1 = (uint16_t)(data[128+off+10] | (data[128+off+11]<<8));
                    uint32_t idx = (uint32_t)data[128+off+12] | ((uint32_t)data[128+off+13]<<8) |
                                   ((uint32_t)data[128+off+14]<<16) | ((uint32_t)data[128+off+15]<<24);
                    Vec3d col0 = rgb565_to_rgb_d(c0), col1 = rgb565_to_rgb_d(c1);
                    Vec3d cols[4] = { col0, col1,
                        {floor((2*col0.r+col1.r)/3), floor((2*col0.g+col1.g)/3), floor((2*col0.b+col1.b)/3)},
                        {floor((col0.r+2*col1.r)/3), floor((col0.g+2*col1.g)/3), floor((col0.b+2*col1.b)/3)} };
                    for (int i = 0; i < 16; i++) {
                        int csel = (idx >> (2*i)) & 0x3;
                        int asel = (int)((alpha_bits >> (3*i)) & 0x7);
                        block_rgba[i][0]=(unsigned char)cols[csel].r; block_rgba[i][1]=(unsigned char)cols[csel].g;
                        block_rgba[i][2]=(unsigned char)cols[csel].b; block_rgba[i][3]=alphas[asel];
                    }
                } else {
                    uint16_t c0 = (uint16_t)(data[128+off] | (data[128+off+1]<<8));
                    uint16_t c1 = (uint16_t)(data[128+off+2] | (data[128+off+3]<<8));
                    uint32_t idx = (uint32_t)data[128+off+4] | ((uint32_t)data[128+off+5]<<8) |
                                   ((uint32_t)data[128+off+6]<<16) | ((uint32_t)data[128+off+7]<<24);
                    Vec3d col0 = rgb565_to_rgb_d(c0), col1 = rgb565_to_rgb_d(c1);
                    Vec3d cols[4] = { col0, col1,
                        {floor((2*col0.r+col1.r)/3), floor((2*col0.g+col1.g)/3), floor((2*col0.b+col1.b)/3)},
                        {floor((col0.r+2*col1.r)/3), floor((col0.g+2*col1.g)/3), floor((col0.b+2*col1.b)/3)} };
                    for (int i = 0; i < 16; i++) {
                        int csel = (idx >> (2*i)) & 0x3;
                        block_rgba[i][0]=(unsigned char)cols[csel].r; block_rgba[i][1]=(unsigned char)cols[csel].g;
                        block_rgba[i][2]=(unsigned char)cols[csel].b; block_rgba[i][3]=255;
                    }
                }
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx*4 + (i%4), y = by*4 + (i/4);
                    if (x < w && y < h) {
                        size_t pidx = ((size_t)y*w+x)*4;
                        memcpy(rgba+pidx, block_rgba[i], 4);
                    }
                }
            }
        }
        free(data);
        *out_rgba = rgba; *out_w = w; *out_h = h;
        return 0;
    }

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

/* Looks for "<prefix>_<index>.dds" (case-insensitive) in input_dir.
 * out_found_png is set if a same-numbered .png exists instead (so the
 * caller can report "PNG not supported yet" instead of "not found"). */
static int find_replacement_ps3(const char *input_dir, const char *prefix, int index,
                                 char *out_path, size_t out_path_size, int *out_found_png) {
    *out_found_png = 0;
    snprintf(out_path, out_path_size, "%s/%s_%d.dds", input_dir, prefix, index);
    FILE *f = fopen(out_path, "rb");
    if (f) { fclose(f); return 0; }

    char png_path[1200];
    snprintf(png_path, sizeof(png_path), "%s/%s_%d.png", input_dir, prefix, index);
    FILE *pf = fopen(png_path, "rb");
    if (pf) { fclose(pf); *out_found_png = 1; }
    return -1;
}

/* --------------------------------------------------------------------
 * Public entry point. Ported from repack() (Phase 1 scope, matching
 * the reference script's own stated "first working version" scope):
 * replace existing textures at their ORIGINAL resolution, format
 * changes allowed (following the replacement DDS's own FourCC).
 * NOT yet covered: cubemap replacement (kept unchanged, same as the
 * reference's default/protected behavior), new texture slot creation,
 * PNG replacement input.
 * ------------------------------------------------------------------ */

int ps3_texpack_repack(const char *thb_path, const char *tbb_path,
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

    /* New .tbb data is built up fresh (unlike unpack, entries get
     * relocated/resized, so we can't just patch the original in place). */
    unsigned char *new_tbb = (unsigned char *)malloc(1);
    size_t new_tbb_size = 0, new_tbb_cap = 1;
    unsigned char **new_descs = (unsigned char **)calloc((size_t)count, sizeof(unsigned char *));
    size_t *new_desc_lens = (size_t *)calloc((size_t)count, sizeof(size_t));
    uint32_t *new_tbb_offsets = (uint32_t *)calloc((size_t)count, sizeof(uint32_t));
    uint32_t *new_tex_sizes = (uint32_t *)calloc((size_t)count, sizeof(uint32_t));
    TszipEntry *tszip_entries = (TszipEntry *)calloc((size_t)count, sizeof(TszipEntry));
    int tszip_entry_count = 0;

    log_append(&log, "%s: %d textures", prefix, count);

    for (int i = 0; i < count; i++) {
        TextureEntry *e = &entries[i];
        int bpp = (e->fmt == FMT_DXT1) ? 8 : ((e->fmt == FMT_DXT5) ? 16 : 0);

        int is_cubemap_slot = (e->fmt != FMT_NONE && !e->has_stream && bpp > 0 &&
                                (uint64_t)e->texture_size >= full_chain_size(e->width, e->height, bpp) * 6);

        char repl_path[1200];
        int found_png = 0;
        int has_repl = (find_replacement_ps3(input_dir, prefix, e->index, repl_path, sizeof(repl_path), &found_png) == 0);

        /* A8R8G8B8 is uncompressed -- re-encoding is a pure byte
         * permutation (lossless), so this doesn't need the DXT1/DXT5
         * quality-tradeoff logic below at all. Streaming combined
         * with this format hasn't been observed in practice, so it
         * isn't supported yet -- falls through to verbatim-copy with
         * a clear warning instead of guessing. */
        if (e->fmt == FMT_ARGB8 && has_repl && !e->has_stream) {
            uint32_t rw = 0, rh = 0, rmips = 0; size_t rlen = 0;
            unsigned char *pixels = read_argb8_dds(repl_path, &rw, &rh, &rmips, &rlen);
            if (!pixels) {
                log_append(&log, "  tex%d: WARNING could not read '%s' as an uncompressed 32bpp "
                           "DDS -- keeping original", e->index, repl_path);
            } else if (rw != e->width || rh != e->height) {
                log_append(&log, "  tex%d: WARNING '%s' is %ux%u but the slot is %ux%u -- "
                           "keeping original", e->index, repl_path, rw, rh, e->width, e->height);
                free(pixels);
            } else {
                /* Preserve whatever trailing bytes followed the
                 * decoded pixel data in the ORIGINAL .tbb -- their
                 * purpose isn't understood, so they're carried
                 * forward untouched rather than guessed at. */
                uint64_t orig_decoded = argb8_chain_size(e->width, e->height, e->mips);
                uint64_t trailing_len = (e->texture_size > orig_decoded) ? e->texture_size - orig_decoded : 0;
                const unsigned char *trailing = orig_tbb + e->tbb_offset + orig_decoded;
                if ((uint64_t)e->tbb_offset + orig_decoded + trailing_len > tbb_size) trailing_len = 0;

                /* read_argb8_dds already converted DDS (BGRA, linear)
                 * back to native ARGB byte order; if the ORIGINAL
                 * texture was swizzled, the replacement needs to be
                 * re-swizzled too, matching the same per-level
                 * layout the real hardware expects. Do this in place,
                 * one level at a time, before writing to the tbb. */
                unsigned char *native = pixels;
                if (!e->is_linear) {
                    native = (unsigned char *)malloc((size_t)rlen);
                    uint32_t lw = rw, lh = rh;
                    size_t pos = 0;
                    for (uint32_t m = 0; m < (rmips < 1 ? 1 : rmips); m++) {
                        size_t lvl_len = (size_t)lw * lh * 4;
                        swizzle_transpose_argb8(pixels + pos, native + pos, lw, lh, 0);
                        pos += lvl_len;
                        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
                    }
                }

                uint32_t new_off = (uint32_t)new_tbb_size;
                size_t needed = new_tbb_size + rlen + trailing_len;
                if (needed > new_tbb_cap) {
                    while (new_tbb_cap < needed) new_tbb_cap *= 2;
                    new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap);
                }
                memcpy(new_tbb + new_tbb_size, native, rlen);
                new_tbb_size += rlen;
                if (trailing_len > 0) {
                    memcpy(new_tbb + new_tbb_size, trailing, (size_t)trailing_len);
                    new_tbb_size += (size_t)trailing_len;
                }
                while (new_tbb_size % 128 != 0) {
                    if (new_tbb_size + 1 > new_tbb_cap) { new_tbb_cap *= 2; new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap); }
                    new_tbb[new_tbb_size++] = 0;
                }
                new_tbb_offsets[i] = new_off;
                new_tex_sizes[i] = (uint32_t)(rlen + trailing_len);

                uint32_t extra_fields = read_be32(thb_data, e->thb_offset);
                size_t desc_len = 36 + (extra_fields > 1 ? (extra_fields - 1) : 0) * 4;
                if (e->thb_offset + desc_len > thb_size) desc_len = 36;
                unsigned char *desc = (unsigned char *)malloc(desc_len);
                memcpy(desc, thb_data + e->thb_offset, desc_len);
                uint32_t fmt_and_mips = read_be32(desc, 8);
                /* Preserve the original format byte exactly (0x85
                 * swizzled or 0xA5 linear) -- never force one or the
                 * other, since native's layout above already matches
                 * whichever it was. */
                uint32_t orig_fmt_byte = (fmt_and_mips >> 24) & 0xFF;
                fmt_and_mips = (fmt_and_mips & 0x00FFFFFFu) | (orig_fmt_byte << 24);
                fmt_and_mips = (fmt_and_mips & 0xFF00FFFFu) | ((rmips & 0xFF) << 16);
                write_be32_local(desc, 8, fmt_and_mips);
                new_descs[i] = desc;
                new_desc_lens[i] = desc_len;

                int changed = (rlen != orig_decoded) ||
                    memcmp(native, orig_tbb + e->tbb_offset, (size_t)(rlen < orig_decoded ? rlen : orig_decoded)) != 0;
                log_append(&log, "  tex%d: A8R8G8B8 %ux%u mips=%u <- %s%s", e->index, rw, rh, rmips,
                           repl_path, changed ? "  ** CHANGED **" : "  (unchanged)");
                if (native != pixels) free(native);
                free(pixels);
                continue;
            }
        }

        /* Verbatim-copy path: no replacement, cubemap slot (deferred),
         * or unrecognized format -- copy original bytes unchanged. */
        if (!has_repl || is_cubemap_slot || e->fmt == FMT_NONE || e->fmt == FMT_ARGB8) {
            if (has_repl && is_cubemap_slot)
                log_append(&log, "  tex%d: found a replacement but this is a cubemap slot "
                           "(not supported yet) -- keeping original", e->index);
            else if (!has_repl && found_png)
                log_append(&log, "  tex%d: found a .png replacement, but PNG input isn't "
                           "supported yet (use .dds) -- keeping original", e->index);
            else if (e->fmt == FMT_ARGB8 && has_repl && e->has_stream)
                log_append(&log, "  tex%d: found a replacement, but streamed A8R8G8B8 isn't "
                           "supported yet -- keeping original", e->index);

            size_t chunk_len = e->texture_size;
            if ((uint64_t)e->tbb_offset + chunk_len > tbb_size) chunk_len = 0;
            uint32_t new_off = (uint32_t)new_tbb_size;
            size_t needed = new_tbb_size + chunk_len;
            if (needed > new_tbb_cap) {
                while (new_tbb_cap < needed) new_tbb_cap *= 2;
                new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap);
            }
            if (chunk_len > 0)
                memcpy(new_tbb + new_tbb_size, orig_tbb + e->tbb_offset, chunk_len);
            new_tbb_size += chunk_len;
            while (new_tbb_size % 128 != 0) {
                if (new_tbb_size + 1 > new_tbb_cap) { new_tbb_cap *= 2; new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap); }
                new_tbb[new_tbb_size++] = 0;
            }

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
                    /* Fall back to a loose .tstream file, first in
                     * input_dir, then next to the .tbb. */
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
                } else if (!orig_tszip_path) {
                    log_append(&log, "  tex%d: WARNING has_stream=true but no stream found "
                               "(no original .tszip given, and no loose .tstream file next to "
                               "the .tbb) -- this texture's high-res data will be lost",
                               e->index);
                }
                free(actual_name);
            }

            uint32_t extra_fields = read_be32(thb_data, e->thb_offset);
            size_t desc_len = 36 + (extra_fields > 1 ? (extra_fields - 1) : 0) * 4;
            if (e->thb_offset + desc_len > thb_size) desc_len = 36;
            unsigned char *desc = (unsigned char *)malloc(desc_len);
            memcpy(desc, thb_data + e->thb_offset, desc_len);
            new_descs[i] = desc;
            new_desc_lens[i] = desc_len;
            new_tbb_offsets[i] = new_off;
            new_tex_sizes[i] = (uint32_t)chunk_len;
            log_append(&log, "  tex%d: kept original (%s %ux%u)", e->index,
                       e->fmt == FMT_DXT1 ? "DXT1" : (e->fmt == FMT_DXT5 ? "DXT5" :
                       (e->fmt == FMT_ARGB8 ? "A8R8G8B8" : "unrecognized format")),
                       e->width, e->height);
            continue;
        }

        /* Replace: decode the replacement (following ITS OWN format if
         * it's a recognizable DDS, else falling back to the slot's
         * original format), re-encode the full mip chain at the
         * ORIGINAL resolution. */
        TexFormat target_fmt = detect_replacement_format(repl_path);
        if (target_fmt == FMT_NONE) target_fmt = e->fmt;
        if (target_fmt != e->fmt)
            log_append(&log, "  tex%d: replacement is %s, slot was %s -- repacking this slot as %s",
                       e->index, target_fmt == FMT_DXT1 ? "DXT1" : "DXT5",
                       e->fmt == FMT_DXT1 ? "DXT1" : "DXT5",
                       target_fmt == FMT_DXT1 ? "DXT1" : "DXT5");

        unsigned char *rgba = NULL; uint32_t rw = 0, rh = 0;
        char *load_err = NULL;
        if (load_replacement_dds_ps3(repl_path, &rgba, &rw, &rh, &load_err) != 0) {
            log_append(&log, "  tex%d: WARNING could not load replacement '%s' (%s) -- unchanged",
                       e->index, repl_path, load_err ? load_err : "unknown error");
            free(load_err);
            /* fall through to verbatim-copy behavior for this entry */
            size_t chunk_len = e->texture_size;
            if ((uint64_t)e->tbb_offset + chunk_len > tbb_size) chunk_len = 0;
            uint32_t new_off = (uint32_t)new_tbb_size;
            size_t needed = new_tbb_size + chunk_len;
            if (needed > new_tbb_cap) { while (new_tbb_cap < needed) new_tbb_cap *= 2; new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap); }
            if (chunk_len > 0) memcpy(new_tbb + new_tbb_size, orig_tbb + e->tbb_offset, chunk_len);
            new_tbb_size += chunk_len;
            while (new_tbb_size % 128 != 0) {
                if (new_tbb_size + 1 > new_tbb_cap) { new_tbb_cap *= 2; new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap); }
                new_tbb[new_tbb_size++] = 0;
            }
            uint32_t extra_fields = read_be32(thb_data, e->thb_offset);
            size_t desc_len = 36 + (extra_fields > 1 ? (extra_fields - 1) : 0) * 4;
            unsigned char *desc = (unsigned char *)malloc(desc_len);
            memcpy(desc, thb_data + e->thb_offset, desc_len);
            new_descs[i] = desc; new_desc_lens[i] = desc_len;
            new_tbb_offsets[i] = new_off; new_tex_sizes[i] = (uint32_t)chunk_len;
            continue;
        }

        if (rw != e->width || rh != e->height) {
            unsigned char *resized = box_downsample_ps3(rgba, rw, rh, e->width, e->height);
            free(rgba);
            rgba = resized;
            log_append(&log, "  tex%d: replacement was %ux%u, resized to %ux%u",
                       e->index, rw, rh, e->width, e->height);
        }

        size_t full_data_len = 0;
        unsigned char *full_data = encode_full_chain_ps3(rgba, e->width, e->height, target_fmt, &full_data_len);
        free(rgba);

        int new_bpp = (target_fmt == FMT_DXT1) ? 8 : 16;
        unsigned char *stream_bytes = NULL; size_t stream_len = 0;
        unsigned char *tbb_bytes = full_data; size_t tbb_bytes_len = full_data_len;

        if (e->has_stream) {
            /* split_point_bytes: offset where the larger dimension first
             * drops to <=128. */
            size_t split = 0;
            uint32_t cw = e->width, ch = e->height;
            while ((cw > ch ? cw : ch) > 128) {
                uint32_t bw = (cw + 3) / 4, bh = (ch + 3) / 4;
                split += (size_t)bw * bh * (size_t)new_bpp;
                cw = cw / 2 > 1 ? cw / 2 : 1;
                ch = ch / 2 > 1 ? ch / 2 : 1;
            }
            stream_bytes = full_data;
            stream_len = split;
            tbb_bytes = full_data + split;
            tbb_bytes_len = full_data_len - split;
        }

        uint32_t new_off = (uint32_t)new_tbb_size;
        size_t needed = new_tbb_size + tbb_bytes_len;
        if (needed > new_tbb_cap) { while (new_tbb_cap < needed) new_tbb_cap *= 2; new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap); }
        memcpy(new_tbb + new_tbb_size, tbb_bytes, tbb_bytes_len);
        new_tbb_size += tbb_bytes_len;
        while (new_tbb_size % 128 != 0) {
            if (new_tbb_size + 1 > new_tbb_cap) { new_tbb_cap *= 2; new_tbb = (unsigned char *)realloc(new_tbb, new_tbb_cap); }
            new_tbb[new_tbb_size++] = 0;
        }

        if (e->has_stream) {
            TszipEntry *te = &tszip_entries[tszip_entry_count++];
            snprintf(te->name, sizeof(te->name), "%s_%d.tstream", prefix, e->index);
            te->data = (unsigned char *)malloc(stream_len);
            memcpy(te->data, stream_bytes, stream_len);
            te->size = stream_len;
        }

        /* Rebuild the descriptor: recompute total_size/mip-count/format
         * byte, keep every other field copied from the original. */
        uint32_t extra_fields = read_be32(thb_data, e->thb_offset);
        uint32_t old_fmt_mips = read_be32(thb_data, e->thb_offset + 8);
        uint32_t unk1 = read_be32(thb_data, e->thb_offset + 12);
        uint32_t wh = read_be32(thb_data, e->thb_offset + 16);
        uint32_t unk2 = read_be32(thb_data, e->thb_offset + 20);
        uint32_t unk3 = read_be32(thb_data, e->thb_offset + 24);
        uint32_t unk4 = read_be32(thb_data, e->thb_offset + 28);
        uint32_t reserved = read_be32(thb_data, e->thb_offset + 32);

        int mips = 1;
        { uint32_t cw = e->width, ch = e->height;
          while (!(cw == 1 && ch == 1)) { cw = cw/2>1?cw/2:1; ch = ch/2>1?ch/2:1; mips++; } }
        uint32_t top_byte = (target_fmt == FMT_DXT5 ? 0x88u : 0x86u) << 24;
        uint32_t new_fmt_mips = top_byte | ((uint32_t)mips << 16) | (old_fmt_mips & 0xFFFFu);

        /* offsets table: cumulative per-level byte sizes while the
         * larger dimension is still >128 -- same loop as the split
         * point, but recording every intermediate total. */
        uint32_t offsets[32]; int n_offsets = 0;
        if (e->has_stream) {
            uint64_t cum = 0;
            uint32_t cw = e->width, ch = e->height;
            while ((cw > ch ? cw : ch) > 128) {
                uint32_t bw = (cw + 3) / 4, bh = (ch + 3) / 4;
                cum += (uint64_t)bw * bh * (uint64_t)new_bpp;
                offsets[n_offsets++] = (uint32_t)cum;
                cw = cw/2>1?cw/2:1; ch = ch/2>1?ch/2:1;
            }
        }

        size_t desc_len = 36 + (size_t)n_offsets * 4;
        unsigned char *desc = (unsigned char *)malloc(desc_len);
        write_be32_local(desc, 0, extra_fields);
        write_be32_local(desc, 4, (uint32_t)tbb_bytes_len + (uint32_t)stream_len); /* total_size */
        write_be32_local(desc, 8, new_fmt_mips);
        write_be32_local(desc, 12, unk1);
        write_be32_local(desc, 16, wh);
        write_be32_local(desc, 20, unk2);
        write_be32_local(desc, 24, unk3);
        write_be32_local(desc, 28, unk4);
        write_be32_local(desc, 32, reserved);
        for (int k = 0; k < n_offsets; k++)
            write_be32_local(desc, 36 + k * 4, offsets[k]);

        new_descs[i] = desc;
        new_desc_lens[i] = desc_len;
        new_tbb_offsets[i] = new_off;
        new_tex_sizes[i] = (uint32_t)tbb_bytes_len;

        free(full_data);
        log_append(&log, "  tex%d: REPLACED with %s (%s %ux%u)", e->index,
                   repl_path, target_fmt == FMT_DXT1 ? "DXT1" : "DXT5", e->width, e->height);
    }

    /* Lay out the new .thb: count, then count 12-byte entries, then
     * descriptors packed back-to-back immediately after. */
    size_t header_area_size = 4 + (size_t)count * 12;
    uint32_t *desc_offsets = (uint32_t *)calloc((size_t)count, sizeof(uint32_t));
    size_t running = header_area_size;
    for (int i = 0; i < count; i++) {
        desc_offsets[i] = (uint32_t)running;
        running += new_desc_lens[i];
    }

    size_t out_thb_size = running;
    unsigned char *out_thb = (unsigned char *)malloc(out_thb_size);
    write_be32_local(out_thb, 0, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        size_t base = 4 + (size_t)i * 12;
        write_be32_local(out_thb, base + 0, desc_offsets[i]);
        write_be32_local(out_thb, base + 4, new_tbb_offsets[i]);
        write_be32_local(out_thb, base + 8, new_tex_sizes[i]);
    }
    for (int i = 0; i < count; i++)
        memcpy(out_thb + desc_offsets[i], new_descs[i], new_desc_lens[i]);

    char out_thb_path[1200], out_tbb_path[1200];
    snprintf(out_thb_path, sizeof(out_thb_path), "%s/%s.thb", out_dir, prefix);
    snprintf(out_tbb_path, sizeof(out_tbb_path), "%s/%s.tbb", out_dir, prefix);

    FILE *tf = fopen(out_tbb_path, "wb");
    int tbb_ok = 0;
    if (tf) { size_t w = fwrite(new_tbb, 1, new_tbb_size, tf); fclose(tf); tbb_ok = (w == new_tbb_size); }
    FILE *thf = fopen(out_thb_path, "wb");
    int thb_ok = 0;
    if (thf) { size_t w = fwrite(out_thb, 1, out_thb_size, thf); fclose(thf); thb_ok = (w == out_thb_size); }

    if (thb_ok) log_append(&log, "wrote %s (%lu bytes)", out_thb_path, (unsigned long)out_thb_size);
    else log_append(&log, "WARNING could not write %s", out_thb_path);
    if (tbb_ok) log_append(&log, "wrote %s (%lu bytes)", out_tbb_path, (unsigned long)new_tbb_size);
    else log_append(&log, "WARNING could not write %s", out_tbb_path);

    if (tszip_entry_count > 0) {
        char prefix_upper[256];
        size_t plen = strlen(prefix);
        if (plen >= sizeof(prefix_upper)) plen = sizeof(prefix_upper) - 1;
        for (size_t k = 0; k < plen; k++)
            prefix_upper[k] = (char)toupper((unsigned char)prefix[k]);
        prefix_upper[plen] = '\0';

        char tszip_path[1200];
        snprintf(tszip_path, sizeof(tszip_path), "%s/%s.TSZIP", out_dir, prefix_upper);
        if (build_tszip(tszip_entries, tszip_entry_count, tszip_path) == 0)
            log_append(&log, "wrote %s (%d streamed textures)", tszip_path, tszip_entry_count);
        else
            log_append(&log, "WARNING could not write %s", tszip_path);
    } else {
        log_append(&log, "no streamed textures -- .tszip not written");
    }

    for (int i = 0; i < tszip_entry_count; i++) free(tszip_entries[i].data);
    free(tszip_entries);
    for (int i = 0; i < count; i++) free(new_descs[i]);
    free(new_descs); free(new_desc_lens); free(new_tbb_offsets); free(new_tex_sizes);
    free(desc_offsets);
    free(out_thb);
    free(new_tbb);
    free(entries);
    free(thb_data);
    free(orig_tbb);

    *out_log = log.data;
    return 0;
}

