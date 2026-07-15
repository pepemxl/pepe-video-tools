#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml>

#include "app/mediapoolmodel.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("PepeVideo Studio");
    QGuiApplication::setOrganizationName("Pepe");

    // Modelo del Media Pool, expuesto a QML como singleton PepeVideo.MediaPoolModel.
    MediaPoolModel mediaPool;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "MediaPoolModel", &mediaPool);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("PepeVideo", "Main");

    return app.exec();
}
