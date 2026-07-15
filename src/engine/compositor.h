#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QVector>

#include "../app/timelinemodel.h" // TimelineModel::RenderClip

class FrameGrabber;
class QThread;
class QTimer;

using RenderClipList = QVector<TimelineModel::RenderClip>;

// Worker de composición: vive en un hilo de trabajo propio, posee los FrameGrabber
// (cache por ruta) y produce el fotograma compuesto. Solo recibe datos por valor
// (RenderClipList), nunca toca el TimelineModel (que vive en el hilo de GUI).
class CompositorWorker : public QObject
{
    Q_OBJECT
public:
    explicit CompositorWorker(QSize outSize, QObject *parent = nullptr);
    ~CompositorWorker() override;

public slots:
    void composeFrame(const RenderClipList &clips);

signals:
    void frameReady(const QImage &image, bool hasContent);

private:
    FrameGrabber *grabberFor(const QString &path);

    QHash<QString, FrameGrabber *> m_grabbers;
    QSize m_outSize;
};

// Compositor multicapa del monitor de PROGRAMA (fachada en el hilo de GUI).
// Mantiene el reloj de reproducción y el estado; en cada composición toma una
// instantánea de los clips activos (TimelineModel::clipsAt) y la envía al worker.
// Control busy/pending: como mucho una composición en vuelo; si llega otra petición
// mientras el worker trabaja, se recompone al terminar con el playhead más reciente
// (descarte de fotogramas, latencia acotada).
class Compositor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY hasContentChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
public:
    explicit Compositor(QObject *parent = nullptr);
    ~Compositor() override;

    void setTimeline(TimelineModel *timeline);
    bool hasContent() const { return m_hasContent; }
    bool playing() const { return m_playing; }

    // Transporte del monitor de PROGRAMA (reproduce la timeline en tiempo real).
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();

signals:
    void frameReady(const QImage &image);
    void hasContentChanged();
    void playingChanged();

    // Petición al worker (conexión encolada entre hilos).
    void requestCompose(const RenderClipList &clips);

private:
    void scheduleComposite();
    void requestComposite();
    void onWorkerFrame(const QImage &image, bool hasContent);
    void tick();

    TimelineModel *m_timeline = nullptr;
    QThread *m_thread = nullptr;
    CompositorWorker *m_worker = nullptr;

    QTimer *m_debounce = nullptr;
    QTimer *m_clock = nullptr;         // reloj de reproducción (~30 Hz)
    QElapsedTimer m_wall;              // tiempo real transcurrido
    qint64 m_lastWallMs = 0;

    bool m_hasContent = false;
    bool m_playing = false;
    bool m_busy = false;               // hay una composición en vuelo en el worker
    bool m_pending = false;            // llegó otra petición mientras estaba ocupado
};
