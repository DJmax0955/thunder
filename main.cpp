#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include "dds_image_provider.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("dds"), new DdsImageProvider);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("Thunder", "Main");

    return QGuiApplication::exec();
}
