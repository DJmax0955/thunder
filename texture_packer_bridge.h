#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <qqmlintegration.h>

// Thin QML-callable wrapper around the plain-C platform backends
// (ps3_texpack.c, x360_texpack.c, ...). This class only marshals
// QString <-> C strings and calls into the real logic -- it doesn't
// contain any format logic itself.
class TexturePackerBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit TexturePackerBridge(QObject *parent = nullptr);

    // Returns a human-readable, newline-separated log of what happened
    // (one line per texture). If something fatal went wrong (e.g. the
    // .thb couldn't be parsed at all), the returned string starts with
    // "ERROR: ".
    //
    // tszipPath should point at the .tszip archive holding this file's
    // "<prefix>_<index>.tstream" entries.
    Q_INVOKABLE QString unpackPs3(const QString &thbPath,
                                   const QString &tbbPath,
                                   const QString &outDir,
                                   const QString &tszipPath);

    // Repacks using replacement DDS files found in inputDir (named
    // "<prefix>_<index>.dds"). Covers flat (non-cubemap) DXT1/DXT5
    // textures, with format-changing support (following the
    // replacement's own FourCC). NOT yet covered: cubemap repack, new
    // texture slots, PNG replacement input. tszipPath is the ORIGINAL
    // .tszip to carry forward unedited streamed textures from -- pass
    // an empty string if there isn't one (any has_stream texture with
    // no loose .tstream file next to the .tbb is then silently
    // dropped from the output, so this matters whenever the source
    // file actually uses streaming).
    Q_INVOKABLE QString repackPs3(const QString &thbPath,
                                   const QString &tbbPath,
                                   const QString &inputDir,
                                   const QString &outDir,
                                   const QString &tszipPath);

    // Same contract as unpackPs3, for the Xbox 360 format. Covers
    // DXT1/DXT5 flat and cubemap textures; CTX1/ARGB8 entries are
    // skipped with a log line explaining why (not yet ported).
    // fixGamma applies the X360 hardware's gamma correction curve to
    // flat (non-cubemap) textures -- pass true for correct-looking
    // colors matching the arcade/PC/PS3 versions.
    Q_INVOKABLE QString unpackX360(const QString &thbPath,
                                    const QString &tbbPath,
                                    const QString &outDir,
                                    const QString &tszipPath,
                                    bool fixGamma);

    // Repacks using replacement DDS files found in inputDir (named
    // "<prefix>_<index>.dds"). Covers flat (non-cubemap) DXT1/DXT5/CTX1
    // textures. NOT yet covered: cubemap repack, PNG replacement input.
    // tszipPath is the ORIGINAL .tszip to carry forward unedited
    // streamed textures' mip0 from -- pass an empty string if there
    // isn't one (any has_stream texture with no replacement and no
    // loose .tstream file next to the .tbb is then silently dropped
    // from the output, so this matters whenever the source file
    // actually uses streaming).
    Q_INVOKABLE QString repackX360(const QString &thbPath,
                                    const QString &tbbPath,
                                    const QString &inputDir,
                                    const QString &outDir,
                                    const QString &tszipPath);

    // Returns the absolute paths of every ".dds" file directly inside
    // folderPath (not recursive), sorted alphabetically. Empty list if
    // the folder doesn't exist or has no .dds files -- never an error,
    // since "nothing unpacked yet" is an expected, normal state.
    Q_INVOKABLE QStringList listDdsFiles(const QString &folderPath);

    // Same contract as unpackPs3/unpackX360, for the Wii format. Covers
    // CMPR/RGBA8/RGB565/RGB5A3 (every format the reference script
    // supports). tszipPath should point at the .tszip archive holding
    // this file's .tstream entries.
    Q_INVOKABLE QString unpackWii(const QString &thbPath,
                                   const QString &tbbPath,
                                   const QString &outDir,
                                   const QString &tszipPath);

    // Repacks using replacement DDS files found in inputDir (named
    // "<prefix>_<index>.dds"). Covers CMPR/RGBA8 slots (the only ones
    // the reference script itself has an encoder for). Unmodified
    // streamed slots have their original .tstream data carried
    // forward from origTszipPath unchanged. NOT yet covered: new
    // texture slots, PNG replacement input.
    Q_INVOKABLE QString repackWii(const QString &thbPath,
                                   const QString &tbbPath,
                                   const QString &inputDir,
                                   const QString &outDir,
                                   const QString &origTszipPath);
};
