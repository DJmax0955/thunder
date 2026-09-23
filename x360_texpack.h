#ifndef X360_TEXPACK_H
#define X360_TEXPACK_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Xbox 360 (Xenon/Octane engine) .thb/.tbb/.tstream texture unpacker.
 *
 * Ported from x360_thb_tbb_extractv5.py's parse_thb()/unpack(). Unlike
 * PS3, this needs real work: the .tbb stores texture data tiled in the
 * Xenos GPU's swizzled memory layout (not linear/row-major), so we have
 * to untile every block using the real Microsoft XGAddress2DTiledOffset
 * formula, plus fix up a 16-bit byte-swap the 360 applies.
 *
 * COVERED in this build: DXT1/DXT5 flat textures (with or without a
 * .tstream), DXT1/DXT5 6-face cubemaps, the --fix-gamma correction for
 * the X360 hardware's gamma curve, and CTX1 (green+alpha detail/normal-
 * style data, decoded to uncompressed RGBA -- never gamma-corrected,
 * since it isn't colour data).
 *
 * NOT YET PORTED (still Python-only):
 *   - "missing .tstream, best-effort recovery from .tbb" (the Python
 *     script itself notes the real reference tool has no fallback here
 *     either -- it warns and skips, same as this build does)
 *   - ARGB8 format (unsupported in the Python source too)
 *   - repack()
 */

/*
 * Unpacks every DXT1/DXT5 texture in thb_path/tbb_path to
 * "<prefix>_<index>.dds" files inside out_dir (created if needed).
 * Textures in an unsupported format (CTX1, ARGB8, anything unrecognized)
 * are skipped with a log line explaining why, not written at all.
 *
 * thb_path, tbb_path : input file paths (must exist).
 * out_dir             : output directory (created if needed).
 * prefix              : output filename prefix. Pass NULL to derive it
 *                        from thb_path's filename.
 * tszip_path          : path to a .tszip archive holding
 *                        "<prefix>_<index>.tstream" entries. Pass NULL
 *                        to fall back to stream_dir instead.
 * stream_dir           : directory to look for "<prefix>_<index>.tstream"
 *                        files in, used only if tszip_path is NULL. Pass
 *                        NULL (with tszip_path also NULL) to default to
 *                        tbb_path's own directory.
 * fix_gamma           : if non-zero, DXT1/DXT5 flat (non-cubemap)
 *                        textures are decoded to RGBA, corrected with
 *                        the X360 gamma LUT, and written as uncompressed
 *                        RGBA DDS instead of the compressed DXT DDS.
 *
 * out_log, out_error_message : same contract as ps3_texpack_unpack().
 *
 * Returns 0 on success (even with some per-texture warnings/skips),
 * non-zero if out_error_message was set.
 */
int x360_texpack_unpack(const char *thb_path, const char *tbb_path,
                         const char *out_dir, const char *prefix,
                         const char *tszip_path, const char *stream_dir,
                         int fix_gamma,
                         char **out_log, char **out_error_message);

/* Frees a string returned in out_log or out_error_message above. */
void x360_texpack_free(char *s);

/*
 * Repacks textures using replacement DDS files found in input_dir
 * (named "<prefix>_<index>.dds", same convention x360_texpack_unpack
 * produces so you can edit the exact file you were given). Entries with
 * no matching replacement are left untouched. Writes a new
 * "<prefix>.tbb" (patched) and "<prefix>.thb" (copied unchanged, since
 * dimensions/formats never change) into out_dir; .tstream files for
 * streamed entries are written there too.
 *
 * Covers flat (non-cubemap) DXT1/DXT5/CTX1 textures, both .tstream and
 * tbb-only. NOT yet covered: cubemap repack, PNG replacement input
 * (DDS only for now).
 *
 * Mip levels below mip0 are generated with a box filter (simple 2x2
 * average), not the Python reference's PIL LANCZOS -- this only affects
 * the smaller/lower mip levels, never mip0 itself.
 *
 * thb_path, tbb_path : input file paths (must exist).
 * input_dir           : directory to look for "<prefix>_<index>.dds"
 *                        replacement files in.
 * out_dir             : output directory (created if needed).
 * prefix              : output filename prefix. Pass NULL to derive it
 *                        from thb_path's filename.
 *
 * out_log, out_error_message : same contract as x360_texpack_unpack().
 *
 * Returns 0 on success (even with some per-texture skips), non-zero if
 * out_error_message was set.
 *
 * orig_tszip_path: the ORIGINAL .tszip to read unedited streamed
 * textures' mip0 from (needed to carry them forward at all --
 * without this, any has_stream texture with no replacement AND no
 * loose .tstream file next to the .tbb is silently dropped from the
 * output .tszip). Pass NULL if there isn't one. Matched by exact
 * "_<index>.tstream" suffix, not by this file's own derived prefix --
 * real files can share one .tszip across resolution variants.
 */
int x360_texpack_repack(const char *thb_path, const char *tbb_path,
                         const char *input_dir, const char *out_dir,
                         const char *prefix, const char *orig_tszip_path,
                         char **out_log, char **out_error_message);

#ifdef __cplusplus
}
#endif

#endif /* X360_TEXPACK_H */
