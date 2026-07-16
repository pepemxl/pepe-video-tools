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
    Q_PROPERTY(QString presetName READ presetName NOTIFY settingsChanged)
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
    QString presetName() const { return m_preset; }

    Q_INVOKABLE void setResolution(int w, int h);
    void setOutFps(double fps);
    void setVideoMbps(int mbps);
    // Aplica un preset con nombre (resolución + fps + bitrate de una vez).
    Q_INVOKABLE void applyPreset(const QString &name, int w, int h, double fps, int mbps);

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
    void requestRun(const ExportJob &job);

private:
    void setStatus(const QString &s);
    void bumpCustom();   // marca el preset como "Personalizado" y notifica

    TimelineModel *m_timeline = nullptr;
    QThread *m_thread = nullptr;
    ExportWorker *m_worker = nullptr;
    bool m_running = false;
    double m_progress = 0.0;
    QString m_status;
    int m_outW = 1920, m_outH = 1080;
    double m_outFps = 30.0;
    int m_mbps = 12;
    QString m_preset = QStringLiteral("YouTube 1080p");
};

// Auto-test de exportación (PVS_EXPORT_SELFTEST): exporta ~2 s a un MP4 temporal y
// verifica que el archivo existe y tiene tamaño. Devuelve 0/1/-1 (no solicitado).
int runExportSelfTestIfRequested();
