#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml>

#include "app/mediapoolmodel.h"
#include "app/theme.h"
#include "app/timelinemodel.h"
#include "app/videocontroller.h"
#include "engine/audioengine.h"
#include "engine/compositor.h"
#include "engine/scopesprovider.h"
#include "engine/scopeview.h"
#include "engine/videosurface.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("PepeVideo Studio");
    QGuiApplication::setOrganizationName("Pepe");

    // Auto-test de audio (decode + remuestreo + medición de pico), sin GUI ni dispositivo.
    if (const int rc = runAudioSelfTestIfRequested(); rc >= 0)
        return rc;

    // Tipos y singletons expuestos a QML en el módulo PepeVideo.
    Theme theme;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Theme", &theme);

    MediaPoolModel mediaPool;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "MediaPoolModel", &mediaPool);

    VideoController videoController;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "VideoController", &videoController);

    TimelineModel timelineModel;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "TimelineModel", &timelineModel);

    Compositor compositor;
    compositor.setTimeline(&timelineModel);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Compositor", &compositor);

    ScopesProvider scopes;
    scopes.setSource(&compositor);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Scopes", &scopes);

    // Motor de audio: mezcla multi-clip del PROGRAMA sincronizada con el transporte.
    AudioEngine audio;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Audio", &audio);

    // Construye la lista de clips de la mezcla desde el timeline y la envía al motor.
    auto rebuildMix = [&audio, &timelineModel]() {
        QVector<AudioMixClip> mix;
        for (const TimelineModel::AudioClip &a : timelineModel.audioClips()) {
            AudioMixClip m;
            m.mediaPath = a.mediaPath;
            m.trackIndex = a.trackIndex;
            m.startUs = a.startUs;
            m.durationUs = a.durationUs;
            m.inUs = a.inUs;
            m.speed = a.speed;
            m.gain = a.gain;
            m.pan = a.pan;
            m.mute = a.mute;
            for (const TimelineModel::Keyframe &k : a.gainKf)
                m.gainKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.panKf)
                m.panKf.push_back({ k.sourceUs, k.value });
            mix.push_back(m);
        }
        audio.setMixData(mix, timelineModel.contentEndUs());
    };
    rebuildMix();
    // Rehornea la mezcla cuando cambia la estructura o algún parámetro de audio.
    QObject::connect(&timelineModel, &TimelineModel::changed, &audio, rebuildMix);
    QObject::connect(&timelineModel, &TimelineModel::audioChanged, &audio, rebuildMix);

    // Sincroniza el audio con play/pause del PROGRAMA, arrancando desde el playhead actual.
    QObject::connect(&compositor, &Compositor::playingChanged, &audio,
                     [&compositor, &audio, &timelineModel]() {
                         if (compositor.playing())
                             audio.playFrom(timelineModel.playheadUs() / 1000);
                         else
                             audio.stop();
                     });

    qmlRegisterType<VideoSurface>("PepeVideo", 1, 0, "VideoSurface");
    qmlRegisterType<ScopeView>("PepeVideo", 1, 0, "ScopeView");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    // Fuerza la creación del singleton QML `Theme` ANTES de cargar la UI. Los
    // singletons basados en archivo QML se crean de forma perezosa en el primer
    // acceso; si ese primer acceso ocurre dentro de un binding durante la
    // construcción del árbol (como pasa aquí, todo se crea en una pasada
    // síncrona), esa primera evaluación devuelve `undefined` y genera cientos de
    // avisos "Unable to assign [undefined]" antes de reevaluarse. Instanciarlo
    // aquí lo deja disponible desde el primer binding.
    engine.loadFromModule("PepeVideo", "Main");

    return app.exec();
}
