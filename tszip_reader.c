#include "tszip_reader.h"
#include "tinflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

static char *dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, s, len);
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

static uint16_t read_le16(const unsigned char *d, size_t off) {
    return (uint16_t)(d[off] | (d[off + 1] << 8));
}

static uint32_t read_le32(const unsigned char *d, size_t off) {
    return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
           ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
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

/* Finds the last signature by scanning backward from the end.
 * This ignores earlier copies or padding, as expected for ZIP files. */
static long find_eocd(const unsigned char *data, size_t size) {
    if (size < 22)
        return -1;
    /* A real ZIP comment can be up to 65535 bytes; search that far back
     * plus the 22-byte fixed EOCD size, or the whole file if smaller. */
    size_t search_limit = (size > 22 + 65535) ? (22 + 65535) : size;
    for (size_t back = 22; back <= search_limit; back++) {
        size_t pos = size - back;
        if (data[pos] == 0x50 && data[pos + 1] == 0x4B &&
            data[pos + 2] == 0x05 && data[pos + 3] == 0x06) {
            return (long)pos;
        }
    }
    return -1;
}

int tszip_read_entry(const char *tszip_path, const char *entry_name,
                      unsigned char **out_data, size_t *out_size,
                      char **out_error) {
    *out_error = NULL;

    unsigned char *file_data = NULL;
    size_t file_size = 0;
    if (read_whole_file(tszip_path, &file_data, &file_size) != 0) {
        *out_error = strdup_fmt("error: could not read '%s'.", tszip_path);
        return -1;
    }

    long eocd_pos = find_eocd(file_data, file_size);
    if (eocd_pos < 0) {
        free(file_data);
        *out_error = strdup_fmt(
            "error: '%s' doesn't look like a valid .tszip (no End-Of-"
            "Central-Directory record found).", tszip_path);
        return -1;
    }

    uint16_t total_entries = read_le16(file_data, (size_t)eocd_pos + 10);
    uint32_t cd_offset = read_le32(file_data, (size_t)eocd_pos + 16);

    if ((uint64_t)cd_offset >= (uint64_t)file_size) {
        free(file_data);
        *out_error = strdup_fmt(
            "error: '%s' has an invalid central-directory offset.", tszip_path);
        return -1;
    }

    size_t entry_name_len = strlen(entry_name);
    size_t pos = cd_offset;

    for (uint16_t i = 0; i < total_entries; i++) {
        if (pos + 46 > file_size) {
            free(file_data);
            *out_error = strdup_fmt(
                "error: '%s' central directory is truncated.", tszip_path);
            return -1;
        }
        uint32_t sig = read_le32(file_data, pos);
        if (sig != 0x02014B50u) {
            free(file_data);
            *out_error = strdup_fmt(
                "error: '%s' central directory entry %u has a bad signature.",
                tszip_path, i);
            return -1;
        }

        uint16_t method = read_le16(file_data, pos + 10);
        uint32_t comp_size = read_le32(file_data, pos + 20);
        uint32_t uncompressed_size = read_le32(file_data, pos + 24);
        uint16_t fname_len = read_le16(file_data, pos + 28);
        uint16_t extra_len = read_le16(file_data, pos + 30);
        uint16_t comment_len = read_le16(file_data, pos + 32);
        uint32_t local_offset = read_le32(file_data, pos + 42);

        size_t name_pos = pos + 46;
        if (name_pos + fname_len > file_size) {
            free(file_data);
            *out_error = strdup_fmt(
                "error: '%s' central directory entry %u is truncated.",
                tszip_path, i);
            return -1;
        }

        int matches = (fname_len == entry_name_len) &&
                      (memcmp(file_data + name_pos, entry_name, fname_len) == 0);

        if (matches) {
            if (method != 0 && method != 8) {
                free(file_data);
                *out_error = strdup_fmt(
                    "error: '%s' inside '%s' uses compression method %u -- "
                    "only STORED (0) and DEFLATE (8) are supported.",
                    entry_name, tszip_path, method);
                return -1;
            }
            if ((uint64_t)local_offset + 30 > (uint64_t)file_size) {
                free(file_data);
                *out_error = strdup_fmt(
                    "error: '%s' has an invalid local header offset for '%s'.",
                    tszip_path, entry_name);
                return -1;
            }
            uint32_t local_sig = read_le32(file_data, local_offset);
            if (local_sig != 0x04034B50u) {
                free(file_data);
                *out_error = strdup_fmt(
                    "error: '%s' local header for '%s' has a bad signature.",
                    tszip_path, entry_name);
                return -1;
            }
            uint16_t local_fname_len = read_le16(file_data, local_offset + 26);
            uint16_t local_extra_len = read_le16(file_data, local_offset + 28);
            uint64_t data_offset = (uint64_t)local_offset + 30 + local_fname_len + local_extra_len;

            if (data_offset + comp_size > (uint64_t)file_size) {
                free(file_data);
                *out_error = strdup_fmt(
                    "error: '%s' entry '%s' data runs past the end of the file.",
                    tszip_path, entry_name);
                return -1;
            }

            if (method == 0) {
                unsigned char *entry_data = (unsigned char *)malloc(comp_size > 0 ? comp_size : 1);
                if (!entry_data) {
                    free(file_data);
                    *out_error = strdup_fmt("error: out of memory.");
                    return -1;
                }
                memcpy(entry_data, file_data + data_offset, comp_size);

                free(file_data);
                *out_data = entry_data;
                *out_size = comp_size;
                return 0;
            }

            /* method == 8: DEFLATE. Decompress with our own dependency-
             * free raw-inflate implementation (tinflate.c). */
            unsigned char *entry_data =
                (unsigned char *)malloc(uncompressed_size > 0 ? uncompressed_size : 1);
            if (!entry_data) {
                free(file_data);
                *out_error = strdup_fmt("error: out of memory.");
                return -1;
            }
            int inflate_rc = tinflate(file_data + data_offset, comp_size,
                                       entry_data, uncompressed_size);
            free(file_data);
            if (inflate_rc != 0) {
                free(entry_data);
                *out_error = strdup_fmt(
                    "error: '%s' inside '%s' failed to decompress (corrupt DEFLATE "
                    "stream or size mismatch).", entry_name, tszip_path);
                return -1;
            }
            *out_data = entry_data;
            *out_size = uncompressed_size;
            return 0;
        }

        pos = name_pos + fname_len + extra_len + comment_len;
    }

    free(file_data);
    *out_error = strdup_fmt("error: '%s' not found inside '%s'.", entry_name, tszip_path);
    return -1;
}

int tszip_list_entries(const char *tszip_path, char ***out_names, int *out_count,
                        char **out_error) {
    *out_error = NULL;

    unsigned char *file_data = NULL;
    size_t file_size = 0;
    if (read_whole_file(tszip_path, &file_data, &file_size) != 0) {
        *out_error = strdup_fmt("error: could not read '%s'.", tszip_path);
        return -1;
    }

    long eocd_pos = find_eocd(file_data, file_size);
    if (eocd_pos < 0) {
        free(file_data);
        *out_error = strdup_fmt(
            "error: '%s' doesn't look like a valid .tszip (no End-Of-"
            "Central-Directory record found).", tszip_path);
        return -1;
    }

    uint16_t total_entries = read_le16(file_data, (size_t)eocd_pos + 10);
    uint32_t cd_offset = read_le32(file_data, (size_t)eocd_pos + 16);
    if ((uint64_t)cd_offset >= (uint64_t)file_size) {
        free(file_data);
        *out_error = strdup_fmt(
            "error: '%s' has an invalid central-directory offset.", tszip_path);
        return -1;
    }

    char **names = (char **)calloc(total_entries > 0 ? total_entries : 1, sizeof(char *));
    size_t pos = cd_offset;

    for (uint16_t i = 0; i < total_entries; i++) {
        if (pos + 46 > file_size) {
            for (uint16_t k = 0; k < i; k++) free(names[k]);
            free(names);
            free(file_data);
            *out_error = strdup_fmt("error: '%s' central directory is truncated.", tszip_path);
            return -1;
        }
        uint32_t sig = read_le32(file_data, pos);
        if (sig != 0x02014B50u) {
            for (uint16_t k = 0; k < i; k++) free(names[k]);
            free(names);
            free(file_data);
            *out_error = strdup_fmt(
                "error: '%s' central directory entry %u has a bad signature.",
                tszip_path, i);
            return -1;
        }
        uint16_t fname_len = read_le16(file_data, pos + 28);
        uint16_t extra_len = read_le16(file_data, pos + 30);
        uint16_t comment_len = read_le16(file_data, pos + 32);
        size_t name_pos = pos + 46;
        if (name_pos + fname_len > file_size) {
            for (uint16_t k = 0; k < i; k++) free(names[k]);
            free(names);
            free(file_data);
            *out_error = strdup_fmt(
                "error: '%s' central directory entry %u is truncated.", tszip_path, i);
            return -1;
        }

        char *name = (char *)malloc((size_t)fname_len + 1);
        memcpy(name, file_data + name_pos, fname_len);
        name[fname_len] = '\0';
        names[i] = name;

        pos = name_pos + fname_len + extra_len + comment_len;
    }

    free(file_data);
    *out_names = names;
    *out_count = (int)total_entries;
    return 0;
}

void tszip_free_entry_list(char **names, int count) {
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
}
