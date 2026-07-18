#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include "../app/timelinemodel.h"

class QThread;

// Un trabajo de exportación: instantánea autónoma tomada en el hilo de GUI y
// entregada al worker (que no toca el TimelineModel). Contiene, por fotograma de
// salida, la lista de RenderClip ya resuelta, y el audio maestro ya horneado.
struct ExportJob {
    QString path;
    int width = 1920;
    int height = 1080;
    double fps = 30.0;
    qint64 durationUs = 0;
    int videoBitrate = 12'000'000;
    int audioBitrate = 192'000;
    QString format = QStringLiteral("h264");   // id de formato (ver tabla en exporter.cpp)
    QVector<QVector<TimelineModel::RenderClip>> frames; // una por fotograma
    QByteArray audioS16;   // PCM S16 estéreo intercalado a 48 kHz (mezcla maestra)
};
Q_DECLARE_METATYPE(ExportJob)

// Worker de exportación (hilo propio): compone cada fotograma con un CompositorWorker
// y codifica vídeo (H.264/libx264) + audio (AAC) a un MP4 con libavformat/libavcodec.
class ExportWorker : public QObject
{
    Q_OBJECT
public slots:
    void run(const ExportJob &job);
signals:
    void progress(double fraction);
    void finished(bool ok, const QString &message);
};

// Fachada expuesta a QML (singleton PepeVideo.Export). Reúne la instantánea en el
// hilo de GUI y lanza la codificación en segundo plano.
class Exporter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // Ajustes de salida (chips de la StatusBar). Cambiarlos manualmente pone el
    // preset en "Personalizado"; applyPreset() fija los tres y el nombre.
    Q_PROPERTY(int outWidth READ outWidth NOTIFY settingsChanged)
    Q_PROPERTY(int outHeight READ outHeight NOTIFY settingsChanged)
    Q_PROPERTY(double outFps READ outFps WRITE setOutFps NOTIFY settingsChanged)
    Q_PROPERTY(int videoMbps READ videoMbps WRITE setVideoMbps NOTIFY settingsChanged)
    // Bitrate de audio en kbps (0 = exportar sin audio).
    Q_PROPERTY(int audioKbps READ audioKbps WRITE setAudioKbps NOTIFY settingsChanged)
    Q_PROPERTY(QString presetName READ presetName NOTIFY settingsChanged)
    // Formato de salida (códec + contenedor). `format` = id; `formatLabel` = texto;
    // `outExt` = extensión; `availableFormats` = lista de {id,label,ext} soportados por
    // este build de FFmpeg. Cambiarlo NO altera resolución/fps/bitrate.
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY settingsChanged)
    Q_PROPERTY(QString formatLabel READ formatLabel NOTIFY settingsChanged)
    Q_PROPERTY(QString outExt READ outExt NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList availableFormats READ availableFormats CONSTANT)
    // El formato actual usa bitrate (H.264/H.265/VP9) o es basado en calidad (ProRes/DNxHR).
    Q_PROPERTY(bool formatUsesBitrate READ formatUsesBitrate NOTIFY settingsChanged)
    // Ruta de salida (página Entregar): carpeta + nombre base (sin extensión).
    Q_PROPERTY(QString outDir READ outDir NOTIFY settingsChanged)
    Q_PROPERTY(QString outName READ outName WRITE setOutName NOTIFY settingsChanged)
    // Cola de render: lista de trabajos {name, path, width, height, fps, mbps,
    // preset, status (0 pendiente·1 en curso·2 hecho·3 error)}.
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(bool queueRunning READ queueRunning NOTIFY queueChanged)
public:
    explicit Exporter(QObject *parent = nullptr);
    ~Exporter() override;

    void setSources(TimelineModel *timeline);

    bool running() const { return m_running; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    int outWidth() const { return m_outW; }
    int outHeight() const { return m_outH; }
    double outFps() const { return m_outFps; }
    int videoMbps() const { return m_mbps; }
    int audioKbps() const { return m_audioKbps; }
    void setAudioKbps(int kbps);
    QString presetName() const { return m_preset; }
    QString format() const { return m_format; }
    QString formatLabel() const;
    QString outExt() const;
    bool formatUsesBitrate() const;
    QVariantList availableFormats() const;
    void setFormat(const QString &id);
    QString outDir() const { return m_outDir; }
    QString outName() const { return m_outName; }
    QVariantList queue() const;
    bool queueRunning() const { return m_queueRunning; }

    Q_INVOKABLE void setResolution(int w, int h);
    void setOutFps(double fps);
    void setVideoMbps(int mbps);
    void setOutName(const QString &name);
    // Aplica un preset con nombre (resolución + fps + bitrate de una vez).
    Q_INVOKABLE void applyPreset(const QString &name, int w, int h, double fps, int mbps);

    // Página Entregar: elegir carpeta+nombre de salida (diálogo nativo),
    // añadir el trabajo actual a la cola, gestionarla y renderizarla en serie.
    Q_INVOKABLE void chooseOutputFile();
    Q_INVOKABLE void enqueueCurrent();
    Q_INVOKABLE void removeFromQueue(int index);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void startQueue();       // renderiza todos los pendientes en serie
    Q_INVOKABLE void exportNow();        // exporta ya a la ruta de salida (o abre diálogo)

    // Exporta la línea de tiempo a un MP4. seconds<=0 exporta todo el contenido.
    // El bitrate de vídeo sale de los ajustes (videoMbps).
    Q_INVOKABLE void exportTimeline(const QString &path, int width, int height,
                                    double fps, double seconds);
    // Diálogo nativo de "guardar como" (Windows) y exporta con los ajustes actuales.
    Q_INVOKABLE void openExportDialog();

signals:
    void runningChanged();
    void progressChanged();
    void statusChanged();
    void settingsChanged();
    void queueChanged();
    void requestRun(const ExportJob &job);

private:
    void setStatus(const QString &s);
    void bumpCustom();   // marca el preset como "Personalizado" y notifica
    // Un trabajo de la cola de render.
    struct QueueItem {
        QString name, path, preset, format;
        int width, height, mbps, audioKbps;
        double fps;
        qint64 startUs = 0, durUs = 0;   // rango de exportación (marcas I/O)
        int status = 0;   // 0 pendiente · 1 en curso · 2 hecho · 3 error
    };
    // Construye la instantánea del trabajo (fotogramas + audio) del rango
    // [startUs, startUs+durUs). durUs<=0 usa hasta el fin del contenido. audioKbps<=0
    // exporta sin audio. Devuelve false si el rango está vacío.
    bool buildJob(const QString &path, int w, int h, double fps, int mbps, int audioKbps,
                  qint64 startUs, qint64 durUs, ExportJob &out);
    void renderNextInQueue();            // arranca el siguiente pendiente (o termina)
    QString absoluteOutputPath() const;  // outDir/outName.mp4 (o solo el nombre)

    TimelineModel *m_timeline = nullptr;
    QThread *m_thread = nullptr;
    ExportWorker *m_worker = nullptr;
    bool m_running = false;
    double m_progress = 0.0;
    QString m_status;
    int m_outW = 1920, m_outH = 1080;
    double m_outFps = 30.0;
    int m_mbps = 12;
    int m_audioKbps = 192;   // 0 = sin audio
    QString m_preset = QStringLiteral("YouTube 1080p");
    QString m_format = QStringLiteral("h264");
    QString m_outDir;
    QString m_outName = QStringLiteral("pepe_export");
    QVector<QueueItem> m_queue;
    bool m_queueRunning = false;
    int m_curQueueIdx = -1;              // índice del trabajo en curso (-1 = ninguno)
};

// Auto-test de exportación (PVS_EXPORT_SELFTEST): exporta ~2 s a un MP4 temporal y
// verifica que el archivo existe y tiene tamaño. Devuelve 0/1/-1 (no solicitado).
int runExportSelfTestIfRequested();

// Auto-test de la cola de render (PVS_DELIVER_SELFTEST): encola dos trabajos y los
// renderiza en serie con un bucle de eventos, verificando ambos MP4 y el estado
// final. Devuelve 0/1/-1 (no solicitado).
int runDeliverSelfTestIfRequested();

// Auto-test de códecs (PVS_CODEC_SELFTEST): renderiza un clip corto en cada formato
// disponible (H.264/H.265/ProRes/DNxHR/VP9) y verifica el archivo. Devuelve 0/1/-1.
int runCodecSelfTestIfRequested();

// Escribe un MP4 de prueba de 2 s @ 24 fps, 320x180: primer segundo naranja
// (#c06030) y segundo azul (#3060c0). Para los autotests de decodificación.
bool pvsWriteColorTestMp4(const QString &path);
