#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItemGrabResult>
#include <QTimer>
#include <QtQml>
#include <functional>

#include "app/logger.h"
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
    // Auto-test de la cola de render (encola dos trabajos y los renderiza en serie).
    if (const int rc = runDeliverSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test de códecs (renderiza un clip corto en cada formato disponible).
    if (const int rc = runCodecSelfTestIfRequested(); rc >= 0)
        return rc;
    // Auto-test de copia directa (remux sin recodificar de un sub-rango).
    if (const int rc = runCopySelfTestIfRequested(); rc >= 0)
        return rc;

    // Logs de la sesión (solo en ejecuciones normales; los autotests salen antes).
    const QString logPath = pvsInstallLogger();
    qInfo().noquote() << "==== PepeVideo Studio ·" << QT_VERSION_STR
                      << "· inicio" << QDateTime::currentDateTime().toString(Qt::ISODate);
    qInfo().noquote() << "Log:" << (logPath.isEmpty() ? QStringLiteral("(no se pudo crear)") : logPath);
    qInfo().noquote() << "Ejecutable:" << QCoreApplication::applicationDirPath();
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
    // El pool comparte la pila de undo del proyecto: Ctrl+Z global también
    // deshace eliminar medios y las operaciones de bins.
    mediaPool.setUndoStack(timelineModel.undoStack());
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "TimelineModel", &timelineModel);

    // Estado de proyecto: nombre/ruta, guardar/abrir .pvsproj, autoguardado y "sucio".
    ProjectModel project;
    project.setSources(&timelineModel, &mediaPool);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Project", &project);

    Compositor compositor;
    compositor.setTimeline(&timelineModel);
    qmlRegisterSingletonInstance("PepeVideo", 1, 0, "Compositor", &compositor);

    // El reloj del PROGRAMA late a los fps de la secuencia (y los sigue si
    // cambian al abrir otro proyecto o editar los ajustes).
    compositor.setFrameRate(project.seqFps());
    QObject::connect(&project, &ProjectModel::sequenceChanged, &compositor,
                     [&project, &compositor]() { compositor.setFrameRate(project.seqFps()); });

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
            m.eqOn = a.eqOn; m.eqLowDb = a.eqLowDb; m.eqMidDb = a.eqMidDb; m.eqHighDb = a.eqHighDb;
            m.compOn = a.compOn; m.compThreshDb = a.compThreshDb;
            m.compRatio = a.compRatio; m.compMakeupDb = a.compMakeupDb;
            m.gateOn = a.gateOn; m.gateThreshDb = a.gateThreshDb;
            m.deEssOn = a.deEssOn; m.deEssThreshDb = a.deEssThreshDb;
            m.reverbOn = a.reverbOn; m.reverbMix = a.reverbMix; m.reverbSize = a.reverbSize;
            m.clipEqOn = a.clipEqOn; m.clipEqLowDb = a.clipEqLowDb; m.clipEqMidDb = a.clipEqMidDb; m.clipEqHighDb = a.clipEqHighDb;
            m.clipCompOn = a.clipCompOn; m.clipCompThreshDb = a.clipCompThreshDb;
            m.clipCompRatio = a.clipCompRatio; m.clipCompMakeupDb = a.clipCompMakeupDb;
            m.clipGateOn = a.clipGateOn; m.clipGateThreshDb = a.clipGateThreshDb;
            m.clipDeEssOn = a.clipDeEssOn; m.clipDeEssThreshDb = a.clipDeEssThreshDb;
            m.clipReverbOn = a.clipReverbOn; m.clipReverbMix = a.clipReverbMix; m.clipReverbSize = a.clipReverbSize;
            for (const TimelineModel::Keyframe &k : a.eqLowKf)  m.clipEqLowKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.eqMidKf)  m.clipEqMidKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.eqHighKf) m.clipEqHighKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.reverbMixKf) m.clipReverbMixKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.compThreshKf) m.clipCompThreshKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.compRatioKf)  m.clipCompRatioKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.compMakeupKf) m.clipCompMakeupKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.gateThreshKf) m.clipGateThreshKf.push_back({ k.sourceUs, k.value });
            for (const TimelineModel::Keyframe &k : a.deEssThreshKf) m.clipDeEssThreshKf.push_back({ k.sourceUs, k.value });
            mix.push_back(m);
        }
        audio.setMixData(mix, timelineModel.contentEndUs(),
                         timelineModel.masterGain(), timelineModel.masterPan(),
                         timelineModel.masterLimiterOn(), timelineModel.masterCeilingDb());
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

    // Auto-test del PROGRAMA (PVS_PROG_SELFTEST, con ventana): compara los píxeles
    // que la composición GPU pinta en pantalla contra la referencia por CPU
    // (CompositorWorker::renderFrame) en dos playheads del proyecto de demo.
    if (qEnvironmentVariableIsSet("PVS_PROG_SELFTEST")) {
        static int fails = 0;
        auto findProg = [&engine, &compositor]() -> VideoSurface * {
            const auto roots = engine.rootObjects();
            for (QObject *root : roots)
                for (VideoSurface *s : root->findChildren<VideoSurface *>())
                    if (s->source() == &compositor)
                        return s;
            return nullptr;
        };
        // Dos playheads con contenido (el demo puede tener huecos).
        auto pickUs = [&timelineModel](qint64 from) -> qint64 {
            const qint64 end = timelineModel.contentEndUs();
            for (qint64 us = from; us < end; us += qMax<qint64>(1, end / 24))
                if (!timelineModel.clipsAt(us).isEmpty())
                    return us;
            return -1;
        };
        // Muestra la referencia CPU y el grab GPU en el mismo punto del lienzo.
        auto compareAt = [&timelineModel](const QImage &grab, qint64 us,
                                          double fx, double fy, const char *what) {
            CompositorWorker ref(QSize(1280, 720));
            bool hc = false;
            const QImage cpu = ref.renderFrame(timelineModel.clipsAt(us), hc);
            if (!hc || cpu.isNull() || grab.isNull()) {
                qInfo("[PROG-SELFTEST] %-38s sin fotograma  FALLO", what);
                ++fails;
                return;
            }
            const QColor a = cpu.pixelColor(int((cpu.width() - 1) * fx),
                                            int((cpu.height() - 1) * fy));
            // El lienzo ocupa el letterbox central del ítem.
            const QSizeF scaled = QSizeF(1280, 720).scaled(grab.size(), Qt::KeepAspectRatio);
            const QPointF org((grab.width() - scaled.width()) / 2.0,
                              (grab.height() - scaled.height()) / 2.0);
            const QColor b = grab.pixelColor(int(org.x() + (scaled.width() - 1) * fx),
                                             int(org.y() + (scaled.height() - 1) * fy));
            const bool ok = qAbs(a.red() - b.red()) < 30 && qAbs(a.green() - b.green()) < 30
                            && qAbs(a.blue() - b.blue()) < 30;
            qInfo("[PROG-SELFTEST] %-38s CPU(%d,%d,%d) GPU(%d,%d,%d)  %s", what,
                  a.red(), a.green(), a.blue(), b.red(), b.green(), b.blue(),
                  ok ? "OK" : "FALLO");
            if (!ok)
                ++fails;
        };
        QTimer::singleShot(1500, &app, [&timelineModel, findProg, pickUs, compareAt]() {
            VideoSurface *surf = findProg();
            const qint64 us1 = pickUs(0);
            if (!surf || us1 < 0) {
                qInfo("[PROG-SELFTEST] sin superficie del PROGRAMA o sin contenido => FALLO");
                QCoreApplication::exit(1);
                return;
            }
            timelineModel.setPlayheadUs(us1);
            QTimer::singleShot(700, surf, [&timelineModel, surf, pickUs, compareAt, us1]() {
                auto grab1 = surf->grabToImage();
                if (!grab1) { QCoreApplication::exit(1); return; }
                QObject::connect(grab1.data(), &QQuickItemGrabResult::ready,
                                 [grab1, &timelineModel, surf, pickUs, compareAt, us1]() {
                    compareAt(grab1->image(), us1, 0.5, 0.5, "playhead A: centro");
                    compareAt(grab1->image(), us1, 0.3, 0.5, "playhead A: (0.3, 0.5)");
                    const qint64 us2 = pickUs(us1 + 1);
                    if (us2 < 0) {
                        qInfo("[PROG-SELFTEST] resultado: %s (%d fallos)",
                              fails ? "FALLO" : "OK", fails);
                        QCoreApplication::exit(fails ? 1 : 0);
                        return;
                    }
                    timelineModel.setPlayheadUs(us2);
                    QTimer::singleShot(700, surf, [&timelineModel, surf, compareAt, us1, us2]() {
                        auto grab2 = surf->grabToImage();
                        if (!grab2) { QCoreApplication::exit(1); return; }
                        QObject::connect(grab2.data(), &QQuickItemGrabResult::ready,
                                         [grab2, &timelineModel, surf, compareAt, us1, us2]() {
                            compareAt(grab2->image(), us2, 0.5, 0.5, "playhead B: centro");
                            compareAt(grab2->image(), us2, 0.7, 0.5, "playhead B: (0.7, 0.5)");

                            // Fase C: etalonaje + opacidad reales — el shader del
                            // material debe coincidir con la LUT por CPU.
                            quint64 pickId = 0;
                            const QVariantList trs = timelineModel.tracks();
                            for (const QVariant &tv : trs) {
                                for (const QVariant &cv : tv.toMap().value("clips").toList()) {
                                    const QVariantMap cm = cv.toMap();
                                    if (cm.value("kind").toString() != QLatin1String("video"))
                                        continue;
                                    const double x = cm.value("x").toDouble();
                                    const double w = cm.value("w").toDouble();
                                    const qint64 s = qint64(x * timelineModel.totalUs());
                                    const qint64 e = qint64((x + w) * timelineModel.totalUs());
                                    if (us1 >= s && us1 < e) {
                                        pickId = cm.value("id").toULongLong();
                                        break;
                                    }
                                }
                                if (pickId)
                                    break;
                            }
                            if (!pickId) {
                                qInfo("[PROG-SELFTEST] resultado: %s (%d fallos)",
                                      fails ? "FALLO" : "OK", fails);
                                QCoreApplication::exit(fails ? 1 : 0);
                                return;
                            }
                            timelineModel.selectClip(pickId);
                            timelineModel.setSelTemp(0.8);
                            timelineModel.setSelSat(0.4);
                            timelineModel.setSelOpacity(0.6);
                            timelineModel.setPlayheadUs(us1);
                            QTimer::singleShot(700, surf, [&timelineModel, surf, compareAt, us1]() {
                                auto grab3 = surf->grabToImage();
                                if (!grab3) { QCoreApplication::exit(1); return; }
                                QObject::connect(grab3.data(), &QQuickItemGrabResult::ready,
                                                 [grab3, &timelineModel, surf, compareAt, us1]() {
                                    compareAt(grab3->image(), us1, 0.5, 0.5,
                                              "etalonaje+opacidad: centro");
                                    compareAt(grab3->image(), us1, 0.35, 0.4,
                                              "etalonaje+opacidad: (0.35, 0.4)");

                                    // Fase D: media real etalonada — el shader gradea
                                    // píxeles decodificados (no rellenos identidad).
                                    const QString mp4 = QDir(QDir::tempPath())
                                        .filePath(QStringLiteral("pvs_prog_selftest.mp4"));
                                    const quint64 mid = pvsWriteColorTestMp4(mp4)
                                        ? timelineModel.addMediaClip(mp4, QStringLiteral("test"),
                                                                     QStringLiteral("video"),
                                                                     2'000'000, 0, 0.9)
                                        : 0;
                                    if (!mid) {
                                        qInfo("[PROG-SELFTEST] fase D: no se pudo insertar el clip  FALLO");
                                        ++fails;
                                        qInfo("[PROG-SELFTEST] resultado: %s (%d fallos)",
                                              fails ? "FALLO" : "OK", fails);
                                        QCoreApplication::exit(1);
                                        return;
                                    }
                                    timelineModel.selectClip(mid);
                                    timelineModel.setSelTemp(0.8);
                                    timelineModel.setSelSat(0.4);
                                    const qint64 us3 = qint64(0.9 * timelineModel.totalUs()) + 200'000;
                                    timelineModel.setPlayheadUs(us3);
                                    QTimer::singleShot(900, surf, [surf, compareAt, us3]() {
                                        auto grab4 = surf->grabToImage();
                                        if (!grab4) { QCoreApplication::exit(1); return; }
                                        QObject::connect(grab4.data(), &QQuickItemGrabResult::ready,
                                                         [grab4, compareAt, us3]() {
                                            compareAt(grab4->image(), us3, 0.5, 0.5,
                                                      "media etalonada: centro");
                                            compareAt(grab4->image(), us3, 0.6, 0.6,
                                                      "media etalonada: (0.6, 0.6)");
                                            qInfo("[PROG-SELFTEST] resultado: %s (%d fallos)",
                                                  fails ? "FALLO" : "OK", fails);
                                            QCoreApplication::exit(fails ? 1 : 0);
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
        QTimer::singleShot(20000, &app, []() {
            qInfo("[PROG-SELFTEST] timeout => FALLO");
            QCoreApplication::exit(1);
        });
    }

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
