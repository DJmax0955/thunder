#include "dds_image_provider.h"
#include "dds_preview.h"

#include <QUrl>
#include <cstring>

DdsImageProvider::DdsImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage DdsImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{

    QString rawId = id;
    int queryPos = rawId.indexOf(QLatin1Char('?'));
    QString pathPart = (queryPos >= 0) ? rawId.left(queryPos) : rawId;

    QString decodedPath = QUrl::fromPercentEncoding(pathPart.toUtf8());
    QByteArray pathUtf8 = decodedPath.toUtf8();

    unsigned char *rgba = nullptr;
    uint32_t w = 0, h = 0;
    char *err = nullptr;
    int rc = dds_preview_load(pathUtf8.constData(), &rgba, &w, &h, &err);
    if (rc != 0) {
        if (err)
            dds_preview_free(err);
        if (size)
            *size = QSize(0, 0);
        return QImage();
    }

    QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
    std::memcpy(img.bits(), rgba, static_cast<size_t>(w) * h * 4);
    dds_preview_free(rgba);

    if (size)
        *size = QSize(static_cast<int>(w), static_cast<int>(h));

    if (requestedSize.isValid() && !requestedSize.isEmpty())
        return img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}
