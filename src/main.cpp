#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml>

#include "app/mediapoolmodel.h"
#include "app/timelinemodel.h"
#include "app/videocontroller.h"
#include "engine/compositor.h"
#include "engine/videosurface.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("PepeVideo Studio");
    QGuiApplication::setOrganizationName("Pepe");

    // Tipos y singletons expuestos a QML en el módulo PepeVideo.
    MediaPoolModel mediaPool;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "MediaPoolModel", &mediaPool);

    VideoController videoController;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "VideoController", &videoController);

    TimelineModel timelineModel;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "TimelineModel", &timelineModel);

    Compositor compositor;
    compositor.setTimeline(&timelineModel);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Compositor", &compositor);

    qmlRegisterType<VideoSurface>("PepeVideo", 1, 0, "VideoSurface");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("PepeVideo", "Main");

    return app.exec();
}
