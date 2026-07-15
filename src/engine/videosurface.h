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
public:
    explicit VideoSurface(QQuickItem *parent = nullptr);

    QObject *source() const { return m_source; }
    void setSource(QObject *source);

    // Se ejecuta en el hilo de render con el hilo de GUI bloqueado: acceso seguro a m_frame.
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

signals:
    void sourceChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private slots:
    void onFrame(const QImage &image);

private:
    QObject *m_source = nullptr;
    QImage m_frame;
    bool m_frameDirty = false;   // hay un fotograma nuevo que subir a textura
};
