#pragma once

#include <QImage>
#include <QQuickItem>

// Superficie de vídeo: sube el fotograma RGBA a una textura GPU y lo compone con el
// Scene Graph de Qt Quick (RHI), centrado y con letterbox. Reemplaza al antiguo
// QQuickPaintedItem que pintaba por CPU.
//
// La fuente es cualquier QObject que emita `frameReady(QImage)` (VideoController para el
// monitor ORIGEN, Compositor para el de PROGRAMA), conectado por firma de señal.
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

private:
    QObject *m_source = nullptr;
    QImage m_frame;
    double m_zoom = 0.0;         // 0 = ajustar; >0 = factor de zoom
    double m_panX = 0.0, m_panY = 0.0;   // paneo (px) con zoom
    bool m_frameDirty = false;   // hay un fotograma nuevo que subir a textura
};
