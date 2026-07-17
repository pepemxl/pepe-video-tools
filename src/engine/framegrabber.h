#pragma once

#include <QCache>
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
    // Devuelve una QImage nula si falla. Los fotogramas decodificados se guardan en
    // una caché LRU por tiempo (FrameCache): recomponer en el mismo playhead — p. ej.
    // al editar transform/color de otra capa — no re-decodifica. Tamaño por defecto
    // 96 MB por archivo; ajustable con PVS_FRAMECACHE_MB (0 = desactivada).
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

    // FrameCache: LRU de fotogramas decodificados, clave = ms pedido, coste = bytes.
    QCache<qint64, QImage> m_cache;
};
