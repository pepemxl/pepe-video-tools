#include "videocontroller.h"
#include "../engine/videodecoder.h"

#include <QFileInfo>
#include <QThread>

VideoController::VideoController(QObject *parent) : QObject(parent)
{
    m_thread = new QThread(this);
    m_decoder = new VideoDecoder;          // sin parent: se mueve de hilo
    m_decoder->moveToThread(m_thread);

    // Comandos (GUI -> decodificador), entregados en el hilo de trabajo.
    connect(this, &VideoController::requestOpen, m_decoder, &VideoDecoder::openFile);
    connect(this, &VideoController::requestPlay, m_decoder, &VideoDecoder::play);
    connect(this, &VideoController::requestPause, m_decoder, &VideoDecoder::pause);
    connect(this, &VideoController::requestSeek, m_decoder, &VideoDecoder::seek);

    // Resultados (decodificador -> GUI), entregados en el hilo de UI.
    connect(m_decoder, &VideoDecoder::opened, this,
            [this](bool ok, qint64 durationMs, int, int, double fps) {
                m_fps = fps;
                m_durationMs = durationMs;
                if (m_hasMedia != ok) { m_hasMedia = ok; emit hasMediaChanged(); }
                emit durationChanged();
                if (ok && m_pendingPlay) { m_pendingPlay = false; play(); }
            });
    connect(m_decoder, &VideoDecoder::frameReady, this,
            [this](const QImage &img, qint64) { emit frameReady(img); });
    connect(m_decoder, &VideoDecoder::positionChanged, this,
            [this](qint64 ms) { if (m_positionMs != ms) { m_positionMs = ms; emit positionChanged(); } });
    connect(m_decoder, &VideoDecoder::playbackFinished, this,
            [this]() { if (m_playing) { m_playing = false; emit playingChanged(); } });

    connect(m_thread, &QThread::finished, m_decoder, &QObject::deleteLater);
    m_thread->start();

    // Autoapertura de demostración para pruebas.
    const QString demo = qEnvironmentVariable("PVS_DEMO_MEDIA");
    if (!demo.isEmpty()) {
        m_pendingPlay = true;   // reproduce en cuanto 'opened' confirme
        open(demo);
    }
}

VideoController::~VideoController()
{
    m_thread->quit();
    m_thread->wait();
}

void VideoController::open(const QString &pathOrUrl)
{
    QString path = pathOrUrl;
    if (path.startsWith(QStringLiteral("file:")))
        path = QUrl(path).toLocalFile();

    m_sourceName = QFileInfo(path).fileName();
    emit sourceChanged();

    m_positionMs = 0;
    emit positionChanged();

    emit requestOpen(path);
}

void VideoController::play()
{
    if (!m_hasMedia)
        return;
    if (!m_playing) { m_playing = true; emit playingChanged(); }
    emit requestPlay();
}

void VideoController::pause()
{
    if (m_playing) { m_playing = false; emit playingChanged(); }
    emit requestPause();
}

void VideoController::togglePlay()
{
    m_playing ? pause() : play();
}

void VideoController::seekMs(qint64 ms)
{
    emit requestSeek(ms);
}

void VideoController::seekFraction(double f)
{
    if (m_durationMs > 0)
        emit requestSeek(qint64(f * m_durationMs));
}

QString VideoController::formatTc(qint64 ms) const
{
    const qint64 totalSec = ms / 1000;
    const int h = int(totalSec / 3600);
    const int m = int((totalSec % 3600) / 60);
    const int s = int(totalSec % 60);
    const double fps = m_fps > 0.0 ? m_fps : 25.0;
    const int f = int((ms % 1000) / 1000.0 * fps);
    return QStringLiteral("%1:%2:%3:%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(f, 2, 10, QLatin1Char('0'));
}

QString VideoController::positionTc() const { return formatTc(m_positionMs); }
QString VideoController::durationTc() const { return formatTc(m_durationMs); }
