#ifndef PS3_TEXPACK_H
#define PS3_TEXPACK_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PS3 .thb/.tbb texture archive unpacker (Cars 2, Octane engine).
 *
 * Ported from ps3_thb_tbb_extract_v6.py's parse_thb()/unpack(). This is
 * pure byte-copying -- no DXT encode/decode needed for unpack, since the
 * .tbb already holds standard linear DXT1/DXT5 data. We just need to
 * locate each texture's bytes and write a 128-byte DDS header in front
 * of them.
 *
 * NOT YET PORTED (still Python-only): repack() -- that needs the real
 * DXT encoder and an image-loading library, which comes in a later phase.
 */

/*
 * Unpacks every texture in thb_path/tbb_path to "<prefix>_<index>.dds"
 * files inside out_dir (created if it doesn't exist). Textures with an
 * unrecognized format byte are written as "<prefix>_<index>.raw" instead
 * (matches the Python script's fallback behavior).
 *
 * thb_path, tbb_path : input file paths (must exist).
 * out_dir             : output directory (created if needed).
 * prefix              : output filename prefix. Pass NULL to derive it
 *                        from thb_path's filename (e.g. "mcqueen.thb"
 *                        -> "mcqueen").
 * tszip_path          : path to a .tszip archive holding
 *                        "<prefix>_<index>.tstream" entries. Pass NULL
 *                        to fall back to stream_dir instead.
 * stream_dir           : directory to look for "<prefix>_<index>.tstream"
 *                        files in, used only if tszip_path is NULL. Pass
 *                        NULL (with tszip_path also NULL) to default to
 *                        tbb_path's own directory.
 *
 * out_log             : on return, points to a malloc'd, newline-joined
 *                        string of one line per texture describing what
 *                        happened (mirrors the Python script's per-texture
 *                        print() lines) -- always set (even to an empty
 *                        string), even if the overall call fails partway
 *                        through. Caller must free with ps3_texpack_free().
 * out_error_message  : on return, NULL if nothing fatal happened (individual
 *                        texture warnings still go to out_log, not here).
 *                        Non-NULL (malloc'd) if the whole operation could
 *                        not proceed at all, e.g. the .thb couldn't be
 *                        parsed. Caller must free with ps3_texpack_free().
 *
 * Returns 0 on success (even with some per-texture warnings), non-zero if
 * out_error_message was set.
 */
int ps3_texpack_unpack(const char *thb_path, const char *tbb_path,
                        const char *out_dir, const char *prefix,
                        const char *tszip_path, const char *stream_dir,
                        char **out_log, char **out_error_message);

/* Frees a string returned in out_log or out_error_message above. */
void ps3_texpack_free(char *s);

/*
 * Repacks textures using replacement DDS files found in input_dir
 * (named "<prefix>_<index>.dds", same convention x360_texpack_unpack
 * produces). Entries with no matching replacement are left untouched.
 * Writes a new "<prefix>.tbb" and "<prefix>.thb" into out_dir; a
 * "<prefix>.tszip" archive is written too if any streamed textures
 * were replaced.
 *
 * Phase 1 scope (matching the reference script's own stated "first
 * working version" scope): replaces existing textures at their
 * ORIGINAL resolution (mismatched-size replacements are box-filter
 * resized to fit). Format changes are supported -- if the replacement
 * DDS declares DXT5 but the slot was DXT1, that slot gets rewritten as
 * DXT5. NOT yet covered: cubemap replacement (always left unchanged),
 * new texture slot creation, PNG replacement input (DDS only for now).
 *
 * Mip levels below mip0 are generated with a box filter, not the
 * Python reference's PIL LANCZOS -- same deliberate simplification as
 * x360_texpack_repack().
 *
 * thb_path, tbb_path : input file paths (must exist).
 * input_dir           : directory to look for "<prefix>_<index>.dds"
 *                        replacement files in.
 * out_dir             : output directory (created if needed).
 * prefix              : output filename prefix. Pass NULL to derive it
 *                        from thb_path's filename.
 * orig_tszip_path     : the ORIGINAL .tszip to read unedited streamed
 *                        textures from (needed to carry them forward
 *                        at all -- without this, any has_stream
 *                        texture with no loose .tstream file sitting
 *                        next to the .tbb is silently dropped from
 *                        the repacked output). Pass NULL if there
 *                        isn't one. Entries are matched by exact
 *                        "_<index>.tstream" suffix, not by this file's
 *                        own derived prefix -- real files can share
 *                        one .tszip across resolution variants (e.g.
 *                        "dolly_high.thb" pulling from "DOLLY.TSZIP",
 *                        whose entries are named "dolly_0.tstream").
 *
 * out_log, out_error_message : same contract as ps3_texpack_unpack().
 *
 * Returns 0 on success (even with some per-texture skips), non-zero if
 * out_error_message was set.
 */
int ps3_texpack_repack(const char *thb_path, const char *tbb_path,
                        const char *input_dir, const char *out_dir,
                        const char *prefix, const char *orig_tszip_path,
                        char **out_log, char **out_error_message);

#ifdef __cplusplus
}
#endif

#endif /* PS3_TEXPACK_H */
