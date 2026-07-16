#include "scopesprovider.h"

#include <QTimer>
#include <QtMath>

ScopesProvider::ScopesProvider(QObject *parent) : QObject(parent)
{
    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    m_throttle->setInterval(66); // ~15 Hz
    connect(m_throttle, &QTimer::timeout, this, [this]() {
        if (m_dirty) { m_dirty = false; compute(); }
    });

    // Autotest opcional (píxeles): imagen roja sólida → histograma y vectorscopio esperados.
    if (qEnvironmentVariableIsSet("PVS_SCOPES_SELFTEST")) {
        QImage red(64, 64, QImage::Format_RGBA8888);
        red.fill(QColor(255, 0, 0));
        m_frame = red;
        compute();
        int pass = 0, fail = 0;
        auto chk = [&](bool ok, const char *w) { if (ok) ++pass; else { ++fail; qWarning("[SCOPES selftest] FALLO: %s", w); } };
        const int bins = m_histogram.size();
        chk(bins == 64, "histograma con 64 bins");
        if (bins == 64) {
            const QVariantList hi = m_histogram.last().toList();   // bin más alto
            const QVariantList lo = m_histogram.first().toList();  // bin más bajo
            chk(hi.at(0).toDouble() > 0.9, "rojo saturado en el bin alto");
            chk(lo.at(1).toDouble() > 0.9 && lo.at(2).toDouble() > 0.9, "verde y azul en el bin bajo");
        }
        // vectorscopio: pico fuera del centro (rojo → esquina superior-izquierda).
        int bx = 128, by = 128; int best = -1;
        for (int y = 0; y < m_vectorscope.height(); ++y)
            for (int x = 0; x < m_vectorscope.width(); ++x) {
                const int g = qGreen(m_vectorscope.pixel(x, y));
                if (g > best) { best = g; bx = x; by = y; }
            }
        chk(qAbs(bx - 128) + qAbs(by - 128) > 40, "vectorscopio: pico del rojo fuera del centro");
        qWarning("[SCOPES selftest] %d OK, %d FALLO", pass, fail);
    }
}

void ScopesProvider::setSource(QObject *frameSource)
{
    if (frameSource)
        connect(frameSource, SIGNAL(frameReady(QImage)), this, SLOT(onFrame(QImage)));
}

void ScopesProvider::onFrame(const QImage &image)
{
    m_frame = image;
    m_dirty = true;
    if (!m_throttle->isActive())
        m_throttle->start();
}

QImage ScopesProvider::scopeImage(const QString &kind) const
{
    if (kind == QLatin1String("waveform"))
        return m_waveform;
    if (kind == QLatin1String("vectorscope"))
        return m_vectorscope;
    return {};
}

void ScopesProvider::compute()
{
    QImage img = m_frame;
    if (img.isNull())
        return;
    if (img.format() != QImage::Format_RGBA8888)
        img = img.convertToFormat(QImage::Format_RGBA8888);

    const int W = img.width(), H = img.height();
    const int bins = 64;
    QVector<double> hr(bins, 0), hg(bins, 0), hb(bins, 0);

    const int wfW = 256, wfH = 256;
    QVector<quint32> wfAcc(wfW * wfH, 0);
    const int vsN = 256;
    QVector<quint32> vsAcc(vsN * vsN, 0);

    // Submuestreo: ~60k muestras independientemente del tamaño.
    const int step = qMax(1, int(std::sqrt(double(qint64(W) * H) / 60000.0)));

    for (int y = 0; y < H; y += step) {
        const uchar *line = img.constScanLine(y);
        for (int x = 0; x < W; x += step) {
            const uchar *px = line + x * 4;
            const int r = px[0], g = px[1], b = px[2];
            hr[r * bins / 256]++; hg[g * bins / 256]++; hb[b * bins / 256]++;

            const int L = (r * 299 + g * 587 + b * 114) / 1000;
            const int wx = x * wfW / W;
            const int wy = (255 - L) * (wfH - 1) / 255;   // brillo arriba
            wfAcc[wy * wfW + wx]++;

            int U = int(-0.169 * r - 0.331 * g + 0.5 * b + 128.0);
            int V = int(0.5 * r - 0.419 * g - 0.081 * b + 128.0);
            U = qBound(0, U, 255);
            V = qBound(0, V, 255);
            vsAcc[(255 - V) * vsN + U]++;                 // V hacia arriba
        }
    }

    // Histograma normalizado por el pico global.
    double maxH = 1.0;
    for (int i = 0; i < bins; ++i)
        maxH = qMax(maxH, qMax(hr[i], qMax(hg[i], hb[i])));
    m_histogram.clear();
    for (int i = 0; i < bins; ++i) {
        QVariantList t;
        t << hr[i] / maxH << hg[i] / maxH << hb[i] / maxH;
        m_histogram.append(QVariant(t));
    }

    // Waveform (verde).
    quint32 wfMax = 1;
    for (quint32 v : wfAcc) wfMax = qMax(wfMax, v);
    m_waveform = QImage(wfW, wfH, QImage::Format_RGBA8888);
    for (int i = 0; i < wfW * wfH; ++i) {
        double a = double(wfAcc[i]) / wfMax;
        a = qMin(1.0, a * 6.0);              // realza trazas tenues
        const int g = int(a * 235) + (a > 0 ? 20 : 0);
        reinterpret_cast<QRgb *>(m_waveform.bits())[i] = qRgba(int(g * 0.4), g, int(g * 0.5), 255);
    }

    // Vectorscopio.
    quint32 vsMax = 1;
    for (quint32 v : vsAcc) vsMax = qMax(vsMax, v);
    m_vectorscope = QImage(vsN, vsN, QImage::Format_RGBA8888);
    for (int i = 0; i < vsN * vsN; ++i) {
        double a = double(vsAcc[i]) / vsMax;
        a = qMin(1.0, a * 6.0);
        const int c = int(a * 255);
        reinterpret_cast<QRgb *>(m_vectorscope.bits())[i] = qRgba(int(c * 0.7), c, int(c * 0.55), 255);
    }

    if (qEnvironmentVariableIsSet("PVS_SCOPES_DEBUG")) {
        static int n = 0;
        ++n;
        if (n <= 2 || n % 15 == 0)
            qInfo("[SCOPES] cálculo #%d desde fotograma %dx%d", n, W, H);
    }

    emit scopesUpdated();
}
