#include "compositor.h"

#include "framegrabber.h"

#include <QColor>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <cmath>

// ===================== Corrección de color (LUT por canal) =====================
void CompositorWorker::gradeImage(QImage &img, const TimelineModel::Color &c)
{
    if (c.isIdentity() || img.isNull())
        return;
    if (img.format() != QImage::Format_RGBA8888)
        img = img.convertToFormat(QImage::Format_RGBA8888);

    // Rueda (x,y) → ajuste por canal: arriba=rojo, abajo-izq=verde, abajo-der=azul.
    auto wheel = [](double x, double y, double &r, double &g, double &b) {
        r = y;
        g = -0.866 * x - 0.5 * y;
        b = 0.866 * x - 0.5 * y;
    };
    double lR, lG, lB, gR, gG, gB, mR, mG, mB;
    wheel(c.liftX, c.liftY, lR, lG, lB);
    wheel(c.gainX, c.gainY, gR, gG, gB);
    wheel(c.gammaX, c.gammaY, mR, mG, mB);

    const double off[3] = { c.temp * 0.2, c.tint * 0.2, -c.temp * 0.2 };  // temp cálido: +R -B
    const double lift[3] = { lR * 0.3, lG * 0.3, lB * 0.3 };
    const double gain[3] = { 1.0 + gR * 0.5, 1.0 + gG * 0.5, 1.0 + gB * 0.5 };
    const double gam[3]  = { 1.0 + mR * 0.5, 1.0 + mG * 0.5, 1.0 + mB * 0.5 };

    quint8 lut[3][256];
    for (int ch = 0; ch < 3; ++ch) {
        const double invGamma = 1.0 / qMax(0.1, gam[ch]);
        for (int i = 0; i < 256; ++i) {
            double v = i / 255.0 + off[ch];
            v = v + lift[ch] * (1.0 - v);   // lift (sombras)
            v = v * gain[ch];               // gain (altas)
            v = qBound(0.0, v, 1.0);
            v = std::pow(v, invGamma);      // gamma (medios)
            lut[ch][i] = quint8(qBound(0.0, v, 1.0) * 255.0 + 0.5);
        }
    }

    const double sat = c.sat;
    const int W = img.width(), H = img.height();
    for (int y = 0; y < H; ++y) {
        uchar *line = img.scanLine(y);
        for (int x = 0; x < W; ++x) {
            uchar *px = line + x * 4;
            int r = lut[0][px[0]], g = lut[1][px[1]], b = lut[2][px[2]];
            if (sat != 1.0) {
                const double L = 0.299 * r + 0.587 * g + 0.114 * b;
                r = qBound(0, int(L + (r - L) * sat + 0.5), 255);
                g = qBound(0, int(L + (g - L) * sat + 0.5), 255);
                b = qBound(0, int(L + (b - L) * sat + 0.5), 255);
            }
            px[0] = uchar(r); px[1] = uchar(g); px[2] = uchar(b);
        }
    }
}

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
    bool hasContent = false;
    QImage img = renderFrame(clips, hasContent);
    emit frameReady(hasContent ? img : QImage(), hasContent);
}

QImage CompositorWorker::renderFrame(const RenderClipList &clips, bool &hasContent)
{
    QImage out(m_outSize, QImage::Format_RGBA8888);
    out.fill(Qt::black);

    bool painted = false;
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const TimelineModel::RenderClip &rc : clips) {
        const TimelineModel::Transform &t = rc.transform;
        QImage frame;
        if (!rc.mediaPath.isEmpty()) {
            if (FrameGrabber *g = grabberFor(rc.mediaPath))
                frame = g->frameAt(rc.sourceUs / 1000);
        }
        if (!frame.isNull())
            gradeImage(frame, rc.color); // corrección de color primaria
        p.setOpacity(t.opacity);
        if (!frame.isNull()) {
            const double fw = frame.width(), fh = frame.height();
            // Recorte del origen (fracciones por borde).
            const double cw = fw * qMax(0.02, 1.0 - t.cropL - t.cropR);
            const double ch = fh * qMax(0.02, 1.0 - t.cropT - t.cropB);
            const QRectF src(t.cropL * fw, t.cropT * fh, cw, ch);
            // Ajuste (fit) del recorte al lienzo, luego escala del usuario.
            const QSizeF fit = QSizeF(cw, ch).scaled(m_outSize, Qt::KeepAspectRatio);
            const QSizeF dst(fit.width() * t.scale, fit.height() * t.scale);
            // Centro + desplazamiento (fracción del lienzo) + rotación.
            const QPointF center(m_outSize.width() * (0.5 + t.posX),
                                 m_outSize.height() * (0.5 + t.posY));
            p.save();
            p.translate(center);
            if (t.rotation != 0.0)
                p.rotate(t.rotation);
            p.drawImage(QRectF(-dst.width() / 2, -dst.height() / 2, dst.width(), dst.height()),
                        frame, src);
            p.restore();
        } else {
            // Sin media: capa de color (placeholder) para visualizar el apilado.
            p.fillRect(out.rect(), QColor(rc.fill));
        }
        p.setOpacity(1.0);
        painted = true;
    }
    p.end();

    hasContent = painted;
    return out;
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

    // Autotest de composición (píxeles): opacidad y mezcla de capas por el camino de color.
    if (qEnvironmentVariableIsSet("PVS_COMP_SELFTEST")) {
        CompositorWorker tester(QSize(64, 64));
        int pass = 0, fail = 0;
        auto chk = [&](bool ok, const char *w) { if (ok) ++pass; else { ++fail; qWarning("[COMP selftest] FALLO: %s", w); } };
        bool hc = false;

        TimelineModel::Transform op;                 // opaco
        QColor a = tester.renderFrame({ { 2, "video", "#ff0000", QString(), 0, op } }, hc).pixelColor(32, 32);
        chk(hc && a.red() > 200 && a.green() < 40 && a.blue() < 40, "color opaco = rojo");

        TimelineModel::Transform o0; o0.opacity = 0.0; // transparente
        QColor b = tester.renderFrame({ { 2, "video", "#ff0000", QString(), 0, o0 } }, hc).pixelColor(32, 32);
        chk(b.red() < 20 && b.green() < 20 && b.blue() < 20, "opacidad 0 = negro");

        TimelineModel::Transform tr;                  // rojo abajo (opaco)
        TimelineModel::Transform tg; tg.opacity = 0.5; // verde arriba al 50%
        QColor c = tester.renderFrame({ { 2, "video", "#ff0000", QString(), 0, tr },
                                        { 0, "video", "#00ff00", QString(), 0, tg } }, hc).pixelColor(32, 32);
        chk(c.red() > 90 && c.red() < 170 && c.green() > 90 && c.green() < 170, "mezcla verde 50% sobre rojo");

        // Corrección de color: saturación 0 → gris; temperatura cálida → +rojo/−azul.
        {
            QImage src(8, 8, QImage::Format_RGBA8888);
            src.fill(QColor(100, 150, 200));
            TimelineModel::Color desat; desat.sat = 0.0;
            QImage g0 = src; CompositorWorker::gradeImage(g0, desat);
            QColor gc = g0.pixelColor(4, 4);
            chk(qAbs(gc.red() - gc.green()) < 3 && qAbs(gc.green() - gc.blue()) < 3, "saturación 0 = gris");

            TimelineModel::Color warm; warm.temp = 1.0;
            QImage gw = src; CompositorWorker::gradeImage(gw, warm);
            QColor wc = gw.pixelColor(4, 4);
            chk(wc.red() > 100 && wc.blue() < 200, "temperatura cálida = +rojo / −azul");
        }

        qWarning("[COMP selftest] %d OK, %d FALLO", pass, fail);
    }
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
        connect(m_timeline, &TimelineModel::selectionChanged, this, &Compositor::scheduleComposite);
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
