#pragma once

#include <QCache>
#include <QImage>
#include <QString>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

// Decodificador sincrónico de un único fotograma, para el compositor: abre un archivo
// y entrega el fotograma RGBA más cercano (<=) a un tiempo dado. Sin hilos ni señales.
//
// Decodificación por hardware (Windows): si el códec lo soporta, el decode corre en el
// GPU vía D3D11VA y el fotograma se copia a RAM (NV12) antes de convertir a RGBA. Si el
// dispositivo o el códec no lo permiten — o falla en pleno archivo — cae a software de
// forma transparente. PVS_HWACCEL=0 lo desactiva por completo.
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
    bool usingHw() const { return m_usingHw; }

    // Fotograma RGBA en el tiempo ms (busca hacia atrás y avanza hasta alcanzarlo).
    // Devuelve una QImage nula si falla. Los fotogramas decodificados se guardan en
    // una caché LRU por tiempo (FrameCache): recomponer en el mismo playhead — p. ej.
    // al editar transform/color de otra capa — no re-decodifica. Tamaño por defecto
    // 96 MB por archivo; ajustable con PVS_FRAMECACHE_MB (0 = desactivada).
    QImage frameAt(qint64 ms);

private:
    void close();
    bool openInternal(const QString &path, bool tryHw);
    bool ensureSws(int w, int h, int srcFormat);
    QImage decodeOne(qint64 &ptsMs);

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVFrame *m_frame = nullptr;
    AVFrame *m_swFrame = nullptr;    // destino de la copia GPU→RAM (NV12)
    AVPacket *m_pkt = nullptr;
    AVBufferRef *m_hwDev = nullptr;  // contexto del dispositivo D3D11VA

    QString m_path;
    int m_videoIndex = -1;
    int m_swsFmt = -1;               // formato de píxel para el que se creó m_sws
    double m_timeBaseSec = 0.0;
    qint64 m_durationMs = 0;
    bool m_eofSent = false;
    bool m_usingHw = false;

    // FrameCache: LRU de fotogramas decodificados, clave = ms pedido, coste = bytes.
    QCache<qint64, QImage> m_cache;
};

// Auto-test (PVS_GRAB_SELFTEST): codifica un MP4 corto con el exportador, lo decodifica
// con y sin hardware y compara. Devuelve 0 (OK) / 1 (fallo) / -1 (no solicitado).
int runGrabSelfTestIfRequested();
