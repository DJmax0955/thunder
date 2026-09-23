#pragma once

#include <QQuickImageProvider>

// Exposes any .dds file as a QML-loadable image via "image://dds/<path>".
// Registered in main.cpp with engine.addImageProvider("dds", ...).
//
// Usage in QML:
//   Image { source: "image://dds/" + encodeURIComponent(absoluteFilePath) }
//
// Decodes with dds_preview.c (plain C, no Qt) -- this class only
// converts the resulting RGBA buffer into a QImage.
class DdsImageProvider : public QQuickImageProvider
{
public:
    DdsImageProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
