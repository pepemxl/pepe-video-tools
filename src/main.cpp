#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItemGrabResult>
#include <QTimer>
#include <QtQml>
#include <functional>

#include "app/mediapoolmodel.h"
#include "app/projectmodel.h"
#include "app/theme.h"
#include "app/timelinemodel.h"
#include "app/videocontroller.h"
#include "engine/audioengine.h"
#include "engine/compositor.h"
#include "engine/exporter.h"
#include "engine/framegrabber.h"
#include "engine/scopesprovider.h"
#include "engine/scopeview.h"
#include "engine/videosurface.h"
#include "engine/waveformprovider.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("PepeVideo Studio");
    QGuiApplication::setOrganizationName("Pepe");

    // Auto-test de audio (decode + remuestreo + medición de pico), sin GUI ni dispositivo.
    if (const int rc = runAudioSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test de exportación (compone + codifica H.264/AAC a un MP4 temporal).
    if (const int rc = runExportSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test de formas de onda (envolvente de PCM real, decodificada en un hilo).
    if (const int rc = runWaveformSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test de proyecto (guardar/abrir/round-trip/dirty).
    if (const int rc = runProjectSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test del Media Pool (filtro + bins).
    if (const int rc = runPoolSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test del FrameGrabber (decode hardware D3D11VA vs software).
    if (const int rc = runGrabSelfTestIfRequested(); rc >= 0)
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

    // Estado de proyecto: nombre/ruta, guardar/abrir .pvsproj, autoguardado y "sucio".
    ProjectModel project;
    project.setSources(&timelineModel, &mediaPool);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Project", &project);

    Compositor compositor;
    compositor.setTimeline(&timelineModel);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Compositor", &compositor);

    ScopesProvider scopes;
    scopes.setSource(&compositor);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Scopes", &scopes);

    // Motor de audio: mezcla multi-clip del PROGRAMA sincronizada con el transporte.
    AudioEngine audio;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Audio", &audio);

    // Formas de onda de los clips de audio (envolvente de PCM real, cacheada por archivo).
    WaveformProvider waveforms;
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Waveforms", &waveforms);

    // Motor de exportación (H.264/AAC → MP4) en segundo plano.
    Exporter exporter;
    exporter.setSources(&timelineModel);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Export", &exporter);

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

    // Hook de prueba: PVS_WORKSPACE=<0..5> arranca en ese workspace del TopBar
    // (0 Medios · 1 Editar · 2 Fusión · 3 Color · 4 Audio · 5 Entregar).
    engine.rootContext()->setContextProperty(
        QStringLiteral("pvsInitialWorkspace"),
        qEnvironmentVariableIsEmpty("PVS_WORKSPACE") ? 1
            : qEnvironmentVariableIntValue("PVS_WORKSPACE"));

    // Fuerza la creación del singleton QML `Theme` ANTES de cargar la UI. Los
    // singletons basados en archivo QML se crean de forma perezosa en el primer
    // acceso; si ese primer acceso ocurre dentro de un binding durante la
    // construcción del árbol (como pasa aquí, todo se crea en una pasada
    // síncrona), esa primera evaluación devuelve `undefined` y genera cientos de
    // avisos "Unable to assign [undefined]" antes de reevaluarse. Instanciarlo
    // aquí lo deja disponible desde el primer binding.
    engine.loadFromModule("PepeVideo", "Main");

    // Auto-test del visor (PVS_YUV_SELFTEST, con ventana): abre un MP4 de prueba en el
    // monitor ORIGEN y comprueba los colores que el material YUV renderiza en pantalla
    // (fotograma naranja en 0 s y azul en 1.5 s). Termina solo con el resultado.
    if (qEnvironmentVariableIsSet("PVS_YUV_SELFTEST")) {
        const QString mp4 = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_yuv_selftest.mp4"));
        if (!pvsWriteColorTestMp4(mp4)) {
            qInfo("[YUV-SELFTEST] no se pudo generar el MP4 de prueba => FALLO");
            return 1;
        }
        videoController.open(mp4);

        static int fails = 0;
        auto approx = [](const QColor &c, int r, int g, int b) {
            return qAbs(c.red() - r) < 32 && qAbs(c.green() - g) < 32 && qAbs(c.blue() - b) < 32;
        };
        auto findSurface = [&engine, &videoController]() -> VideoSurface * {
            const auto roots = engine.rootObjects();
            for (QObject *root : roots)
                for (VideoSurface *s : root->findChildren<VideoSurface *>())
                    if (s->source() == &videoController)
                        return s;
            return nullptr;
        };
        auto grabAndCheck = [approx](VideoSurface *surf, int r, int g, int b,
                                     const char *what, std::function<void()> then) {
            auto grab = surf->grabToImage();
            if (!grab) {
                qInfo("[YUV-SELFTEST] grabToImage falló en '%s' => FALLO", what);
                QCoreApplication::exit(1);
                return;
            }
            QObject::connect(grab.data(), &QQuickItemGrabResult::ready,
                             [grab, approx, r, g, b, what, then]() {
                const QImage img = grab->image();
                const QColor c = img.isNull() ? QColor()
                    : img.pixelColor(img.width() / 2, img.height() / 2);
                const bool ok = !img.isNull() && approx(c, r, g, b);
                qInfo("[YUV-SELFTEST] %-40s (%d,%d,%d)  %s", what,
                      c.red(), c.green(), c.blue(), ok ? "OK" : "FALLO");
                if (!ok) ++fails;
                then();
            });
        };
        QTimer::singleShot(1800, &app, [&videoController, findSurface, grabAndCheck]() {
            VideoSurface *surf = findSurface();
            if (!surf) {
                qInfo("[YUV-SELFTEST] VideoSurface del ORIGEN no encontrada => FALLO");
                QCoreApplication::exit(1);
                return;
            }
            grabAndCheck(surf, 0xc0, 0x60, 0x30, "0.0 s: naranja en pantalla",
                         [&videoController, surf, grabAndCheck]() {
                videoController.seekMs(1500);
                QTimer::singleShot(900, surf, [surf, grabAndCheck]() {
                    grabAndCheck(surf, 0x30, 0x60, 0xc0, "1.5 s: azul en pantalla", []() {
                        qInfo("[YUV-SELFTEST] resultado: %s (%d fallos)",
                              fails ? "FALLO" : "OK", fails);
                        QCoreApplication::exit(fails ? 1 : 0);
                    });
                });
            });
        });
        QTimer::singleShot(15000, &app, []() {
            qInfo("[YUV-SELFTEST] timeout => FALLO");
            QCoreApplication::exit(1);
        });
    }

    return app.exec();
}
