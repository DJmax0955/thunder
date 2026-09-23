#ifndef TSZIP_READER_H
#define TSZIP_READER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal reader for .tszip files: standard ZIP archives (STORED/
 * uncompressed entries only) that hold individual .tstream files.
 *
 * Per the real TSZIP format, the file also has a redundant extra copy of
 * the End-Of-Central-Directory + Central-Directory prepended before the
 * real data ([EOCD][CD][body][CD][EOCD]) -- this reader finds the EOCD
 * by scanning backward from the end of the file, same as any standard
 * ZIP reader, so that front copy is naturally ignored as harmless
 * padding; no special-casing needed.
 *
 * Only STORED (uncompressed) entries are supported -- that's all this
 * format ever uses, so a compressed entry is treated as an error rather
 * than silently needing an inflate implementation.
 */

/*
 * Extracts one named entry's raw bytes from a .tszip archive.
 *
 * tszip_path  : path to the .tszip file.
 * entry_name  : exact filename to look for inside the archive, e.g.
 *               "mcqueen_2.tstream".
 * out_data    : on success, a malloc'd buffer with the entry's bytes.
 *               Caller must free() it.
 * out_size    : on success, the byte length of out_data.
 * out_error   : on failure, a malloc'd error string describing what went
 *               wrong (entry not found, corrupt archive, compressed
 *               entry, etc). Caller must free() it. Set to NULL on
 *               success.
 *
 * Returns 0 on success, non-zero on failure.
 */
int tszip_read_entry(const char *tszip_path, const char *entry_name,
                      unsigned char **out_data, size_t *out_size,
                      char **out_error);

/*
 * Lists every entry name in a .tszip archive, in central-directory
 * order. Needed for platforms (Wii) whose .tstream-to-texture-slot
 * matching is done by sorting filenames rather than looking one up by
 * exact name.
 *
 * out_names : on success, a malloc'd array of out_count malloc'd
 *             strings (caller frees each string, then the array, with
 *             tszip_free_entry_list()).
 * out_count : on success, the number of entries.
 * out_error : on failure, a malloc'd error string (caller frees). NULL
 *             on success.
 *
 * Returns 0 on success, non-zero on failure.
 */
int tszip_list_entries(const char *tszip_path, char ***out_names, int *out_count,
                        char **out_error);

void tszip_free_entry_list(char **names, int count);

#ifdef __cplusplus
}
#endif

#endif /* TSZIP_READER_H */
