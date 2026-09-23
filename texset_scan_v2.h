/* texset_scan - group dropped .thb/.tbb/.tszip paths into complete texture sets.
 *
 * A set is named after the file stem ("ui1" for ui1.thb / ui1.tbb / ui1.tszip).
 * Every set needs BOTH a .thb and a .tbb. The .tszip is optional unless
 * require_tszip is set (the UI's "Has .tstream: Yes"), in which case every set
 * needs one too. Anything missing or duplicated is reported per set - naming
 * which files that set has and which it lacks - instead of being unpacked.
 *
 * Folders are not expanded here - let the UI do that (QDirIterator) and pass the
 * file paths in. Other extensions are ignored.
 */
#ifndef TEXSET_SCAN_V2_H
#define TEXSET_SCAN_V2_H

#define TEXSET_NAME_MAX  256
#define TEXSET_PATH_MAX  1024

typedef struct {
    char name[TEXSET_NAME_MAX];   /* "ui1" */
    char thb[TEXSET_PATH_MAX];
    char tbb[TEXSET_PATH_MAX];
    char tszip[TEXSET_PATH_MAX];  /* "" when the set has no .tszip */
} TexSet;

/* Returns the number of complete sets (>= 0) and stores them in *out_sets, or
 * -1 on failure, storing a multi-line message in *out_error (ready for a
 * QMessageBox). Both outputs are malloc'd; free with texset_free / free().
 * *out_sets is NULL when the count is 0, *out_error is NULL on success. */
int  texset_scan(const char *const *paths, int path_count, int require_tszip,
                 TexSet **out_sets, char **out_error);
void texset_free(TexSet *sets);

#endif
