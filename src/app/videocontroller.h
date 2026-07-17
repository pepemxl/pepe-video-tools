#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include "../engine/videoframe.h"

class QThread;
class VideoDecoder;

// Fachada de reproducción para QML: gestiona el hilo del decodificador,
// expone estado (posición, duración, play) y reenvía fotogramas a la vista.
class VideoController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY hasMediaChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY sourceChanged)
    Q_PROPERTY(QString positionTc READ positionTc NOTIFY positionChanged)
    Q_PROPERTY(QString durationTc READ durationTc NOTIFY durationChanged)
    Q_PROPERTY(double fraction READ fraction NOTIFY positionChanged)

public:
    explicit VideoController(QObject *parent = nullptr);
    ~VideoController() override;

    bool hasMedia() const { return m_hasMedia; }
    bool playing() const { return m_playing; }
    qint64 positionMs() const { return m_positionMs; }
    qint64 durationMs() const { return m_durationMs; }
    QString sourceName() const { return m_sourceName; }
    QString positionTc() const;
    QString durationTc() const;
    double fraction() const { return m_durationMs > 0 ? double(m_positionMs) / m_durationMs : 0.0; }

    Q_INVOKABLE void open(const QString &pathOrUrl);
    // Adopta el ID3D11Device del scene graph (lo reenvía al hilo del decodificador
    // para el decode zero-copy). Lo invoca la VideoSurface desde el hilo de render.
    Q_INVOKABLE void adoptGraphicsDevice(void *d3dDevice);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seekMs(qint64 ms);
    Q_INVOKABLE void seekFraction(double f);
    // Avanza/retrocede `frames` fotogramas (según los fps del medio); pausa primero.
    Q_INVOKABLE void stepFrame(int frames);

signals:
    // Fotograma para la VideoSurface: planos YUV (conversión en GPU) o RGBA de reserva.
    void frameReady(const VideoFrame &frame);
    void hasMediaChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void sourceChanged();

    // Señales internas dirigidas al decodificador (otro hilo).
    void requestOpen(const QString &path);
    void requestAdoptDevice(void *d3dDevice);
    void requestPlay();
    void requestPause();
    void requestSeek(qint64 ms);

private:
    QString formatTc(qint64 ms) const;

    QThread *m_thread = nullptr;
    VideoDecoder *m_decoder = nullptr;

    bool m_hasMedia = false;
    bool m_playing = false;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    double m_fps = 0.0;
    QString m_sourceName;
    bool m_pendingPlay = false;
};
