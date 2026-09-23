#include "texture_packer_bridge.h"
#include "ps3_texpack.h"
#include "x360_texpack.h"
#include "wii_texpack.h"
#include <QDir>
#include <algorithm>

TexturePackerBridge::TexturePackerBridge(QObject *parent) : QObject(parent) {}

QString TexturePackerBridge::unpackPs3(const QString &thbPath, const QString &tbbPath,
                                        const QString &outDir, const QString &tszipPath)
{
    QByteArray thbUtf8 = thbPath.toUtf8();
    QByteArray tbbUtf8 = tbbPath.toUtf8();
    QByteArray outUtf8 = outDir.toUtf8();
    QByteArray tszipUtf8 = tszipPath.toUtf8();

    char *log = nullptr;
    char *err = nullptr;

    int rc = ps3_texpack_unpack(
        thbUtf8.constData(), tbbUtf8.constData(),
        outUtf8.constData(), nullptr /* derive prefix from thbPath */,
        tszipPath.isEmpty() ? nullptr : tszipUtf8.constData(),
        nullptr /* stream_dir: unused when tszipPath is given */,
        &log, &err);

    QString result;
    if (log) {
        result += QString::fromUtf8(log);
        ps3_texpack_free(log);
    }
    if (rc != 0 && err) {
        result += QStringLiteral("ERROR: ") + QString::fromUtf8(err) + QStringLiteral("\n");
        ps3_texpack_free(err);
    } else if (err) {
        ps3_texpack_free(err);
    }
    return result;
}

QString TexturePackerBridge::repackPs3(const QString &thbPath, const QString &tbbPath,
                                        const QString &inputDir, const QString &outDir,
                                        const QString &tszipPath)
{
    QByteArray thbUtf8 = thbPath.toUtf8();
    QByteArray tbbUtf8 = tbbPath.toUtf8();
    QByteArray inputUtf8 = inputDir.toUtf8();
    QByteArray outUtf8 = outDir.toUtf8();
    QByteArray tszipUtf8 = tszipPath.toUtf8();

    char *log = nullptr;
    char *err = nullptr;

    int rc = ps3_texpack_repack(
        thbUtf8.constData(), tbbUtf8.constData(),
        inputUtf8.constData(), outUtf8.constData(),
        nullptr /* derive prefix from thbPath */,
        tszipPath.isEmpty() ? nullptr : tszipUtf8.constData(),
        &log, &err);

    QString result;
    if (log) {
        result += QString::fromUtf8(log);
        ps3_texpack_free(log);
    }
    if (rc != 0 && err) {
        result += QStringLiteral("ERROR: ") + QString::fromUtf8(err) + QStringLiteral("\n");
        ps3_texpack_free(err);
    } else if (err) {
        ps3_texpack_free(err);
    }
    return result;
}

QString TexturePackerBridge::unpackX360(const QString &thbPath, const QString &tbbPath,
                                         const QString &outDir, const QString &tszipPath,
                                         bool fixGamma)
{
    QByteArray thbUtf8 = thbPath.toUtf8();
    QByteArray tbbUtf8 = tbbPath.toUtf8();
    QByteArray outUtf8 = outDir.toUtf8();
    QByteArray tszipUtf8 = tszipPath.toUtf8();

    char *log = nullptr;
    char *err = nullptr;

    int rc = x360_texpack_unpack(
        thbUtf8.constData(), tbbUtf8.constData(),
        outUtf8.constData(), nullptr /* derive prefix from thbPath */,
        tszipPath.isEmpty() ? nullptr : tszipUtf8.constData(),
        nullptr /* stream_dir: unused when tszipPath is given */,
        fixGamma ? 1 : 0,
        &log, &err);

    QString result;
    if (log) {
        result += QString::fromUtf8(log);
        x360_texpack_free(log);
    }
    if (rc != 0 && err) {
        result += QStringLiteral("ERROR: ") + QString::fromUtf8(err) + QStringLiteral("\n");
        x360_texpack_free(err);
    } else if (err) {
        x360_texpack_free(err);
    }
    return result;
}

QString TexturePackerBridge::repackX360(const QString &thbPath, const QString &tbbPath,
                                         const QString &inputDir, const QString &outDir,
                                         const QString &tszipPath)
{
    QByteArray thbUtf8 = thbPath.toUtf8();
    QByteArray tbbUtf8 = tbbPath.toUtf8();
    QByteArray inputUtf8 = inputDir.toUtf8();
    QByteArray outUtf8 = outDir.toUtf8();
    QByteArray tszipUtf8 = tszipPath.toUtf8();

    char *log = nullptr;
    char *err = nullptr;

    int rc = x360_texpack_repack(
        thbUtf8.constData(), tbbUtf8.constData(),
        inputUtf8.constData(), outUtf8.constData(),
        nullptr /* derive prefix from thbPath */,
        tszipPath.isEmpty() ? nullptr : tszipUtf8.constData(),
        &log, &err);

    QString result;
    if (log) {
        result += QString::fromUtf8(log);
        x360_texpack_free(log);
    }
    if (rc != 0 && err) {
        result += QStringLiteral("ERROR: ") + QString::fromUtf8(err) + QStringLiteral("\n");
        x360_texpack_free(err);
    } else if (err) {
        x360_texpack_free(err);
    }
    return result;
}

QStringList TexturePackerBridge::listDdsFiles(const QString &folderPath)
{
    QDir dir(folderPath);
    if (!dir.exists())
        return QStringList();

    QFileInfoList entries = dir.entryInfoList(QStringList() << QStringLiteral("*.dds"),
                                               QDir::Files, QDir::Name);

    // Sort by the numeric "<prefix>_<N>.dds" suffix (0, 1, 2, ..., 10, 11),
    // not plain alphabetical -- alphabetically "10" and "11" sort right
    // after "1", before "2".
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &a, const QFileInfo &b) {
        auto extractIndex = [](const QString &baseName) -> int {
            int underscorePos = baseName.lastIndexOf(QLatin1Char('_'));
            if (underscorePos < 0)
                return -1;
            bool ok = false;
            int value = baseName.mid(underscorePos + 1).toInt(&ok);
            return ok ? value : -1;
        };
        int ia = extractIndex(a.completeBaseName());
        int ib = extractIndex(b.completeBaseName());
        if (ia >= 0 && ib >= 0)
            return ia < ib;
        return a.fileName() < b.fileName();
    });

    QStringList result;
    for (const auto &info : entries)
        result << info.absoluteFilePath();
    return result;
}

QString TexturePackerBridge::unpackWii(const QString &thbPath, const QString &tbbPath,
                                        const QString &outDir, const QString &tszipPath)
{
    QByteArray thbUtf8 = thbPath.toUtf8();
    QByteArray tbbUtf8 = tbbPath.toUtf8();
    QByteArray outUtf8 = outDir.toUtf8();
    QByteArray tszipUtf8 = tszipPath.toUtf8();

    char *log = nullptr;
    char *err = nullptr;

    int rc = wii_texpack_unpack(
        thbUtf8.constData(), tbbUtf8.constData(),
        outUtf8.constData(), nullptr /* derive prefix from thbPath */,
        tszipPath.isEmpty() ? nullptr : tszipUtf8.constData(),
        nullptr /* stream_dir: unused when tszipPath is given */,
        &log, &err);

    QString result;
    if (log) {
        result += QString::fromUtf8(log);
        wii_texpack_free(log);
    }
    if (rc != 0 && err) {
        result += QStringLiteral("ERROR: ") + QString::fromUtf8(err) + QStringLiteral("\n");
        wii_texpack_free(err);
    } else if (err) {
        wii_texpack_free(err);
    }
    return result;
}

QString TexturePackerBridge::repackWii(const QString &thbPath, const QString &tbbPath,
                                        const QString &inputDir, const QString &outDir,
                                        const QString &origTszipPath)
{
    QByteArray thbUtf8 = thbPath.toUtf8();
    QByteArray tbbUtf8 = tbbPath.toUtf8();
    QByteArray inputUtf8 = inputDir.toUtf8();
    QByteArray outUtf8 = outDir.toUtf8();
    QByteArray tszipUtf8 = origTszipPath.toUtf8();

    char *log = nullptr;
    char *err = nullptr;

    int rc = wii_texpack_repack(
        thbUtf8.constData(), tbbUtf8.constData(),
        inputUtf8.constData(), outUtf8.constData(),
        nullptr /* derive prefix from thbPath */,
        origTszipPath.isEmpty() ? nullptr : tszipUtf8.constData(),
        &log, &err);

    QString result;
    if (log) {
        result += QString::fromUtf8(log);
        wii_texpack_free(log);
    }
    if (rc != 0 && err) {
        result += QStringLiteral("ERROR: ") + QString::fromUtf8(err) + QStringLiteral("\n");
        wii_texpack_free(err);
    } else if (err) {
        wii_texpack_free(err);
    }
    return result;
}
