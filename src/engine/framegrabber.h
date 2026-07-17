#pragma once

#include <QCache>
#include <QImage>
#include <QString>

#include "videoframe.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

// Decodificador sincrónico de un único fotograma, para el compositor: abre un archivo
// y entrega el fotograma más cercano (<=) a un tiempo dado. Sin hilos ni señales.
//
// Decodificación por hardware (Windows): si el códec lo soporta, el decode corre en el
// GPU vía D3D11VA; con un dispositivo adoptado (adoptDevice, el del scene graph) la
// sesión es **zero-copy**: el fotograma viaja como textura NV12 en ese dispositivo.
// Sin dispositivo, se copia a RAM como planos I420 (la GPU convierte y etalona en el
// shader); PVS_GPU_YUV=0 fuerza RGBA clásico y PVS_HWACCEL=0 desactiva el hardware.
// Fallback transparente a software en todos los casos.
class FrameGrabber
{
public:
    FrameGrabber() = default;
    ~FrameGrabber();
    FrameGrabber(const FrameGrabber &) = delete;
    FrameGrabber &operator=(const FrameGrabber &) = delete;

    // Adopta el ID3D11Device del scene graph (AddRef; activa la protección
    // multihilo). Debe llamarse ANTES de open(); PVS_ZEROCOPY=0 lo ignora.
    void adoptDevice(void *d3dDevice);

    bool open(const QString &path);
    bool isOpen() const { return m_codec != nullptr; }
    qint64 durationMs() const { return m_durationMs; }
    bool usingHw() const { return m_usingHw; }
    bool zeroCopy() const { return m_zeroCopy; }

    // Fotograma como VideoFrame: textura NV12 nativa (zero-copy), planos I420 o
    // RGBA de reserva. `wantCpuPixels` garantiza píxeles legibles por CPU
    // (re-decodifica con copia a RAM si el caché solo tiene la textura nativa).
    //
    // El tiempo pedido se CUANTIZA a la rejilla de fotogramas del origen (según
    // sus fps): dos peticiones dentro del mismo fotograma comparten clave de
    // caché y no se decodifica dos veces. Peticiones hacia ADELANTE y cercanas
    // (reproducción) continúan el decode secuencial desde la posición actual,
    // sin seek+flush al keyframe anterior — el camino caro queda solo para
    // saltos y retrocesos.
    // FrameCache: LRU por bytes (clave = ms cuantizado; 96 MB por archivo,
    // PVS_FRAMECACHE_MB la ajusta, 0 = off).
    VideoFrame frameVfAt(qint64 ms, bool wantCpuPixels = false);

    // Fotograma RGBA (compositor CPU, exportador, scopes). Convierte los planos
    // cacheados con swscale si hace falta y guarda el resultado en el caché.
    QImage frameAt(qint64 ms);

private:
    void close();
    bool openInternal(const QString &path, bool tryHw);
    qint64 quantizeMs(qint64 ms) const;   // ajusta a la rejilla de fotogramas del origen
    bool ensureSws(int w, int h, int srcFormat);
    bool decodeOne(VideoFrame &out, qint64 &ptsMs, bool forceCpu);
    bool fillNative(VideoFrame &out);
    QImage planesToRgba(const VideoFrame &vf);

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVFrame *m_frame = nullptr;
    AVFrame *m_swFrame = nullptr;    // destino de la copia GPU→RAM (NV12)
    AVPacket *m_pkt = nullptr;
    AVBufferRef *m_hwDev = nullptr;  // contexto del dispositivo D3D11VA
    void *m_extDev = nullptr;        // ID3D11Device* adoptado (AddRef)

    QString m_path;
    int m_videoIndex = -1;
    int m_swsFmt = -1;               // formato de píxel para el que se creó m_sws
    double m_timeBaseSec = 0.0;
    double m_frameDurMs = 0.0;       // duración de un fotograma del origen (0 = desconocida)
    qint64 m_lastPtsMs = -1;         // pts del último fotograma decodificado (posición)
    VideoFrame m_last;               // último fotograma (atajo si aún cubre la petición)
    qint64 m_durationMs = 0;
    bool m_eofSent = false;
    bool m_usingHw = false;
    bool m_zeroCopy = false;

    QCache<qint64, VideoFrame> m_cache;
};

// Auto-test (PVS_GRAB_SELFTEST): codifica un MP4 corto con el exportador, lo decodifica
// con y sin hardware y compara. Devuelve 0 (OK) / 1 (fallo) / -1 (no solicitado).
int runGrabSelfTestIfRequested();
