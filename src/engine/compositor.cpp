#include "compositor.h"

#include "framegrabber.h"

#include <QPainter>
#include <QThread>
#include <QTimer>

// ===================== CompositorWorker (hilo de trabajo) =====================

CompositorWorker::CompositorWorker(QSize outSize, QObject *parent)
    : QObject(parent), m_outSize(outSize)
{
}

CompositorWorker::~CompositorWorker()
{
    qDeleteAll(m_grabbers);
}

FrameGrabber *CompositorWorker::grabberFor(const QString &path)
{
    auto it = m_grabbers.find(path);
    if (it != m_grabbers.end())
        return it.value();

    auto *g = new FrameGrabber;
    if (!g->open(path)) {
        delete g;
        g = nullptr;
    }
    m_grabbers.insert(path, g); // cachea también el fallo (nullptr) para no reintentar
    return g;
}

void CompositorWorker::composeFrame(const RenderClipList &clips)
{
    QImage out(m_outSize, QImage::Format_RGBA8888);
    out.fill(Qt::black);

    bool painted = false;
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const TimelineModel::RenderClip &rc : clips) {
        QImage frame;
        if (!rc.mediaPath.isEmpty()) {
            if (FrameGrabber *g = grabberFor(rc.mediaPath))
                frame = g->frameAt(rc.sourceUs / 1000);
        }
        p.setOpacity(rc.opacity);
        if (!frame.isNull()) {
            const QSize sz = frame.size().scaled(m_outSize, Qt::KeepAspectRatio);
            const QRect tgt(QPoint((m_outSize.width() - sz.width()) / 2,
                                   (m_outSize.height() - sz.height()) / 2), sz);
            p.drawImage(tgt, frame);
        } else {
            // Sin media: capa de color (placeholder) para visualizar el apilado.
            p.fillRect(out.rect(), QColor(rc.fill));
        }
        p.setOpacity(1.0);
        painted = true;
    }
    p.end();

    emit frameReady(painted ? out : QImage(), painted);
}

// ========================= Compositor (hilo de GUI) ===========================

Compositor::Compositor(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<RenderClipList>("RenderClipList");

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(16); // coalesce durante el scrubbing
    connect(m_debounce, &QTimer::timeout, this, &Compositor::requestComposite);

    m_clock = new QTimer(this);
    m_clock->setInterval(33);    // ~30 composiciones/s; el playhead usa tiempo real
    connect(m_clock, &QTimer::timeout, this, &Compositor::tick);

    // Worker en su propio hilo.
    m_thread = new QThread(this);
    m_worker = new CompositorWorker(QSize(1280, 720)); // sin parent: se mueve de hilo
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &Compositor::requestCompose, m_worker, &CompositorWorker::composeFrame);
    connect(m_worker, &CompositorWorker::frameReady, this, &Compositor::onWorkerFrame);
    m_thread->start();
}

Compositor::~Compositor()
{
    m_thread->quit();
    m_thread->wait();
}

void Compositor::setTimeline(TimelineModel *timeline)
{
    if (m_timeline == timeline)
        return;
    if (m_timeline)
        disconnect(m_timeline, nullptr, this, nullptr);
    m_timeline = timeline;
    if (m_timeline) {
        connect(m_timeline, &TimelineModel::playheadChanged, this, &Compositor::scheduleComposite);
        connect(m_timeline, &TimelineModel::changed, this, &Compositor::scheduleComposite);
        scheduleComposite();

        // Hook de prueba: autoarranque de la reproducción del PROGRAMA tras cargar la UI.
        if (qEnvironmentVariableIsSet("PVS_PROGRAM_AUTOPLAY"))
            QTimer::singleShot(600, this, [this]() { play(); });
    }
}

void Compositor::scheduleComposite()
{
    if (m_playing)
        return; // durante la reproducción compone el reloj (tick), no el antirrebote
    m_debounce->start();
}

void Compositor::requestComposite()
{
    if (!m_timeline)
        return;
    if (m_busy) {          // ya hay una composición en vuelo: recompón al terminar
        m_pending = true;
        return;
    }
    m_busy = true;
    emit requestCompose(m_timeline->clipsAt(m_timeline->playheadUs()));
}

void Compositor::onWorkerFrame(const QImage &image, bool hasContent)
{
    m_busy = false;
    if (m_hasContent != hasContent) {
        m_hasContent = hasContent;
        emit hasContentChanged();
    }
    emit frameReady(image);

    if (m_pending) {       // hubo peticiones mientras el worker trabajaba: al día
        m_pending = false;
        requestComposite();
    }
}

void Compositor::play()
{
    if (m_playing || !m_timeline)
        return;
    // Si el playhead está al final del contenido, reinicia desde el principio.
    if (m_timeline->playheadUs() >= m_timeline->contentEndUs())
        m_timeline->setPlayheadUs(0);
    m_wall.restart();
    m_lastWallMs = 0;
    m_playing = true;
    emit playingChanged();
    m_clock->start();
}

void Compositor::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_clock->stop();
    emit playingChanged();
}

void Compositor::togglePlay()
{
    m_playing ? pause() : play();
}

void Compositor::tick()
{
    if (!m_timeline)
        return;
    // Avance por tiempo real: lee el playhead actual (respeta seeks manuales) y le suma
    // el tiempo transcurrido desde el último tick. Si el worker se retrasa, el playhead
    // sigue avanzando y las composiciones intermedias se descartan (busy/pending).
    const qint64 now = m_wall.elapsed();
    const qint64 deltaMs = now - m_lastWallMs;
    m_lastWallMs = now;

    const qint64 end = m_timeline->contentEndUs();
    qint64 us = m_timeline->playheadUs() + deltaMs * 1000;
    bool atEnd = false;
    if (us >= end) { us = end; atEnd = true; }

    m_timeline->setPlayheadUs(us); // mueve la UI; scheduleComposite es no-op mientras playing
    requestComposite();
    if (atEnd)
        pause();
}
