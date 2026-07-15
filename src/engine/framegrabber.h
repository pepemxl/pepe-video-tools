#pragma once

#include <QImage>
#include <QString>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Decodificador sincrónico de un único fotograma, para el compositor: abre un archivo
// y entrega el fotograma RGBA más cercano (<=) a un tiempo dado. Sin hilos ni señales.
class FrameGrabber
{
public:
    FrameGrabber() = default;
    ~FrameGrabber();
    FrameGrabber(const FrameGrabber &) = delete;
    FrameGrabber &operator=(const FrameGrabber &) = delete;

    bool open(const QString &path);
    bool isOpen() const { return m_codec != nullptr; }
    qint64 durationMs() const { return m_durationMs; }

    // Fotograma RGBA en el tiempo ms (busca hacia atrás y avanza hasta alcanzarlo).
    // Devuelve una QImage nula si falla.
    QImage frameAt(qint64 ms);

private:
    void close();
    bool ensureSws(int w, int h, int srcFormat);
    QImage decodeOne(qint64 &ptsMs);

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;

    int m_videoIndex = -1;
    double m_timeBaseSec = 0.0;
    qint64 m_durationMs = 0;
    bool m_eofSent = false;
};
