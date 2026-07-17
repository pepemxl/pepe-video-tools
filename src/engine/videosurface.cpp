#include "videosurface.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

VideoSurface::VideoSurface(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void VideoSurface::setSource(QObject *source)
{
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        // Conexión por firma: cualquier fuente con `frameReady(QImage)` sirve.
        connect(m_source, SIGNAL(frameReady(QImage)), this, SLOT(onFrame(QImage)));
    }
    emit sourceChanged();
}

void VideoSurface::setZoom(double zoom)
{
    zoom = qMax(0.0, zoom);
    if (m_zoom == zoom)   // valores discretos del desplegable: comparación exacta
        return;
    m_zoom = zoom;
    emit zoomChanged();
    update();
}

void VideoSurface::setPanX(double v)
{
    if (m_panX == v)
        return;
    m_panX = v;
    emit panChanged();
    update();
}

void VideoSurface::setPanY(double v)
{
    if (m_panY == v)
        return;
    m_panY = v;
    emit panChanged();
    update();
}

void VideoSurface::onFrame(const QImage &image)
{
    m_frame = image;
    m_frameDirty = true;
    update();
}

void VideoSurface::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update(); // recalcular el letterbox
}

QSGNode *VideoSurface::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (m_frame.isNull() || width() <= 0 || height() <= 0) {
        delete oldNode;
        return nullptr;
    }

    auto *node = static_cast<QSGImageNode *>(oldNode);
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);           // el nodo libera la textura al reemplazarla
        node->setFiltering(QSGTexture::Linear);
    }

    if (m_frameDirty || !node->texture()) {
        QSGTexture *tex = window()->createTextureFromImage(
            m_frame, QQuickWindow::TextureHasAlphaChannel);
        node->setTexture(tex);                // libera la textura anterior (ownsTexture)
        m_frameDirty = false;
        if (qEnvironmentVariableIsSet("PVS_RHI_DEBUG")) {
            static int n = 0;
            ++n;
            if (n <= 3 || n % 30 == 0)
                qInfo("[RHI] textura GPU #%d subida (%dx%d)", n, m_frame.width(), m_frame.height());
        }
    }

    // Ajuste (letterbox KeepAspectRatio) o zoom fijo sobre el tamaño nativo del
    // fotograma; con zoom, 1.0 = un píxel de origen por píxel físico (÷ el DPR).
    const QSizeF area = size();
    QSizeF scaled;
    if (m_zoom > 0.0) {
        const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
        scaled = QSizeF(m_frame.size()) * (m_zoom / dpr);
    } else {
        scaled = QSizeF(m_frame.size()).scaled(area, Qt::KeepAspectRatio);
    }
    // El paneo solo aplica con zoom (en ajuste el fotograma cabe entero).
    const double px = m_zoom > 0.0 ? m_panX : 0.0;
    const double py = m_zoom > 0.0 ? m_panY : 0.0;
    node->setRect(QRectF((area.width() - scaled.width()) / 2.0 + px,
                         (area.height() - scaled.height()) / 2.0 + py,
                         scaled.width(), scaled.height()));
    return node;
}
