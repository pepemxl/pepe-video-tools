#pragma once

#include <QImage>
#include <QQuickPaintedItem>

class VideoController;

// Superficie de vídeo: dibuja el fotograma RGBA actual, centrado y con letterbox.
class VideoSurface : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(VideoController *controller READ controller WRITE setController NOTIFY controllerChanged)
public:
    explicit VideoSurface(QQuickItem *parent = nullptr);

    VideoController *controller() const { return m_controller; }
    void setController(VideoController *controller);

    void paint(QPainter *painter) override;

signals:
    void controllerChanged();

private slots:
    void onFrame(const QImage &image);

private:
    VideoController *m_controller = nullptr;
    QImage m_frame;
};
