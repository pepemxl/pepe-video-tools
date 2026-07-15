#include "videosurface.h"
#include "../app/videocontroller.h"

#include <QPainter>

VideoSurface::VideoSurface(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setFillColor(Qt::black);
}

void VideoSurface::setController(VideoController *controller)
{
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller)
        connect(m_controller, &VideoController::frameReady, this, &VideoSurface::onFrame);
    emit controllerChanged();
}

void VideoSurface::onFrame(const QImage &image)
{
    m_frame = image;
    update();
}

void VideoSurface::paint(QPainter *painter)
{
    if (m_frame.isNull())
        return;

    const QSizeF area = size();
    QSizeF scaled = QSizeF(m_frame.size()).scaled(area, Qt::KeepAspectRatio);
    const QRectF target((area.width() - scaled.width()) / 2.0,
                        (area.height() - scaled.height()) / 2.0,
                        scaled.width(), scaled.height());
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, m_frame);
}
