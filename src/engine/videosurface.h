#pragma once

#include <QImage>
#include <QQuickItem>

#include "videoframe.h"

// Superficie de vídeo: compone el fotograma con el Scene Graph de Qt Quick (RHI),
// centrado y con letterbox.
//
// Dos rutas de subida a GPU:
//  - YUV (monitor ORIGEN): la fuente entrega planos I420 (`frameReady(VideoFrame)`);
//    se suben como tres texturas R8 y la conversión YUV→RGB corre en el fragment
//    shader (YuvMaterial) — sin swscale ni QImage RGBA intermedia.
//  - RGBA (monitor PROGRAMA): la fuente entrega el fotograma ya compuesto
//    (`frameReady(QImage)`) y se sube con una textura clásica (QSGImageNode).
// Con el renderer por software de Qt Quick (sin RHI) la ruta YUV cae a una
// conversión por CPU, ya que los materiales personalizados no están soportados.
class VideoSurface : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)
    // Zoom del visor: 0 = ajustar (letterbox, por defecto); >0 = factor sobre el
    // tamaño nativo del fotograma (1.0 = 100%, un píxel de origen por píxel físico).
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    // Paneo del visor (px del ítem); solo tiene sentido con zoom (>0).
    Q_PROPERTY(double panX READ panX WRITE setPanX NOTIFY panChanged)
    Q_PROPERTY(double panY READ panY WRITE setPanY NOTIFY panChanged)
public:
    explicit VideoSurface(QQuickItem *parent = nullptr);

    QObject *source() const { return m_source; }
    void setSource(QObject *source);
    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    double panX() const { return m_panX; }
    double panY() const { return m_panY; }
    void setPanX(double v);
    void setPanY(double v);

    // Se ejecuta en el hilo de render con el hilo de GUI bloqueado: acceso seguro a m_frame.
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

signals:
    void sourceChanged();
    void zoomChanged();
    void panChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private slots:
    void onFrame(const QImage &image);
    void onVideoFrame(const VideoFrame &frame);

private:
    QRectF targetRect(const QSizeF &frameSize) const;

    QObject *m_source = nullptr;
    QImage m_frame;              // ruta RGBA (compositor / reserva)
    VideoFrame m_yuv;            // ruta YUV (decodificador del ORIGEN)
    double m_zoom = 0.0;         // 0 = ajustar; >0 = factor de zoom
    double m_panX = 0.0, m_panY = 0.0;   // paneo (px) con zoom
    bool m_frameDirty = false;   // hay un fotograma nuevo que subir a textura
    bool m_devSent = false;      // ya se adoptó el device D3D11 hacia la fuente
    int m_nodeKind = 0;          // 0 ninguno · 1 imagen RGBA · 2 material YUV
};
