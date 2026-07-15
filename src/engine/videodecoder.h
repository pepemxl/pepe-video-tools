#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Decodificador de vídeo sobre libav*. Vive en un hilo de trabajo propio:
// demux + decode + conversión a RGBA, con reproducción pautada por PTS.
class VideoDecoder : public QObject
{
    Q_OBJECT
public:
    explicit VideoDecoder(QObject *parent = nullptr);
    ~VideoDecoder() override;

public slots:
    void openFile(const QString &path);
    void play();
    void pause();
    void stop();
    void seek(qint64 ms);

signals:
    void opened(bool ok, qint64 durationMs, int width, int height, double fps);
    void frameReady(const QImage &image, qint64 ptsMs);
    void positionChanged(qint64 ms);
    void playbackFinished();
    void error(const QString &message);

private:
    void closeFile();
    void scheduleNext();
    bool decodeOneFrame(QImage &out, qint64 &ptsMs);
    bool ensureSws(int w, int h, int srcFormat);

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;

    int m_videoIndex = -1;
    int m_width = 0;
    int m_height = 0;
    double m_fps = 0.0;
    qint64 m_durationMs = 0;
    double m_timeBaseSec = 0.0;   // segundos por unidad de PTS

    bool m_playing = false;
    bool m_draining = false;
    qint64 m_lastPtsMs = 0;
    qint64 m_clockStartMs = 0;    // wall.elapsed() - ptsMs
    QElapsedTimer m_wall;
};
