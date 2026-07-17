#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "videoframe.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

// Decodificador de vídeo sobre libav*. Vive en un hilo de trabajo propio:
// demux + decode con reproducción pautada por PTS. Entrega VideoFrame con los
// planos YUV 4:2:0 (la conversión a RGB corre en el GPU, en la VideoSurface);
// para formatos exóticos cae a RGBA por swscale dentro del propio VideoFrame.
//
// Decodificación por hardware (Windows): D3D11VA si el códec lo soporta, con
// fallback transparente a software; PVS_HWACCEL=0 la desactiva y PVS_GPU_YUV=0
// fuerza la ruta RGBA clásica (diagnóstico).
//
// Zero-copy: si la VideoSurface le adopta el dispositivo D3D11 de Qt Quick
// (adoptDevice), el decoder crea el contexto D3D11VA de FFmpeg sobre ese mismo
// dispositivo y entrega cada fotograma como textura NV12 (copia GPU→GPU del
// slice del decodificador) — el fotograma nunca pasa por RAM. Sin dispositivo
// adoptado cae a la copia GPU→RAM (planos I420). PVS_ZEROCOPY=0 lo desactiva.
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
    // Adopta el ID3D11Device del scene graph (AddRef interno). Si hay un medio
    // abierto en pausa con hardware, lo reabre en silencio para activar el zero-copy.
    void adoptDevice(void *d3dDevice);

signals:
    void opened(bool ok, qint64 durationMs, int width, int height, double fps);
    void frameReady(const VideoFrame &frame, qint64 ptsMs);
    void positionChanged(qint64 ms);
    void playbackFinished();
    void error(const QString &message);

private:
    void closeFile();
    bool openImpl(const QString &path, bool tryHw, QString *err);
    void seekInternalOnly(qint64 ms);   // reposiciona sin decodificar ni emitir
    void scheduleNext();
    bool decodeOneFrame(VideoFrame &out, qint64 &ptsMs);
    bool ensureSws(int w, int h, int srcFormat);
    bool fillNativeFrame(VideoFrame &out);   // zero-copy: slice → textura NV12 propia

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVFrame *m_frame = nullptr;
    AVFrame *m_swFrame = nullptr;    // destino de la copia GPU→RAM (NV12)
    AVPacket *m_pkt = nullptr;
    AVBufferRef *m_hwDev = nullptr;  // contexto del dispositivo D3D11VA

    QString m_path;
    int m_videoIndex = -1;
    int m_width = 0;
    int m_height = 0;
    int m_swsFmt = -1;               // formato de píxel para el que se creó m_sws
    double m_fps = 0.0;
    qint64 m_durationMs = 0;
    double m_timeBaseSec = 0.0;   // segundos por unidad de PTS

    void *m_extDev = nullptr;     // ID3D11Device* del scene graph, adoptado (AddRef)

    bool m_playing = false;
    bool m_draining = false;
    bool m_usingHw = false;
    bool m_zeroCopy = false;      // la sesión decodifica en el dispositivo de Qt
    qint64 m_lastPtsMs = 0;
    qint64 m_clockStartMs = 0;    // wall.elapsed() - ptsMs
    QElapsedTimer m_wall;
};
