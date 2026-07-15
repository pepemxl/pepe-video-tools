#include "videodecoder.h"

#include <QTimer>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoDecoder::VideoDecoder(QObject *parent) : QObject(parent) {}

VideoDecoder::~VideoDecoder()
{
    closeFile();
}

void VideoDecoder::closeFile()
{
    m_playing = false;
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); }
    if (m_pkt) { av_packet_free(&m_pkt); }
    if (m_codec) { avcodec_free_context(&m_codec); }
    if (m_fmt) { avformat_close_input(&m_fmt); }
    m_videoIndex = -1;
    m_width = m_height = 0;
    m_fps = 0.0;
    m_durationMs = 0;
    m_lastPtsMs = 0;
    m_draining = false;
}

void VideoDecoder::openFile(const QString &path)
{
    closeFile();

    const QByteArray utf8 = path.toUtf8();
    if (avformat_open_input(&m_fmt, utf8.constData(), nullptr, nullptr) < 0) {
        emit error(QStringLiteral("No se pudo abrir: %1").arg(path));
        emit opened(false, 0, 0, 0, 0.0);
        return;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        emit error(QStringLiteral("Sin información de streams"));
        emit opened(false, 0, 0, 0, 0.0);
        closeFile();
        return;
    }

    m_videoIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoIndex < 0) {
        emit error(QStringLiteral("Sin stream de vídeo"));
        emit opened(false, 0, 0, 0, 0.0);
        closeFile();
        return;
    }

    AVStream *stream = m_fmt->streams[m_videoIndex];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        emit error(QStringLiteral("Códec no soportado"));
        emit opened(false, 0, 0, 0, 0.0);
        closeFile();
        return;
    }

    m_codec = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(m_codec, stream->codecpar);
    m_codec->thread_count = 0; // auto (multihilo de decodificación)
    if (avcodec_open2(m_codec, decoder, nullptr) < 0) {
        emit error(QStringLiteral("No se pudo abrir el códec"));
        emit opened(false, 0, 0, 0, 0.0);
        closeFile();
        return;
    }

    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();

    m_width = m_codec->width;
    m_height = m_codec->height;
    m_timeBaseSec = av_q2d(stream->time_base);
    m_fps = av_q2d(stream->avg_frame_rate);
    if (m_fps <= 0.0)
        m_fps = av_q2d(av_guess_frame_rate(m_fmt, stream, nullptr));

    if (m_fmt->duration > 0)
        m_durationMs = m_fmt->duration / (AV_TIME_BASE / 1000);
    else if (stream->duration > 0)
        m_durationMs = qint64(stream->duration * m_timeBaseSec * 1000.0);

    m_wall.restart();
    m_lastPtsMs = 0;

    emit opened(true, m_durationMs, m_width, m_height, m_fps);

    // Muestra el primer fotograma aunque esté en pausa.
    QImage img;
    qint64 pts = 0;
    if (decodeOneFrame(img, pts)) {
        m_lastPtsMs = pts;
        emit frameReady(img, pts);
        emit positionChanged(pts);
    }
}

bool VideoDecoder::ensureSws(int w, int h, int srcFormat)
{
    if (m_sws)
        return true;
    m_sws = sws_getContext(w, h, static_cast<AVPixelFormat>(srcFormat),
                           w, h, AV_PIX_FMT_RGBA,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    return m_sws != nullptr;
}

bool VideoDecoder::decodeOneFrame(QImage &out, qint64 &ptsMs)
{
    if (!m_codec)
        return false;

    for (;;) {
        int ret = avcodec_receive_frame(m_codec, m_frame);
        if (ret == 0) {
            const int w = m_frame->width;
            const int h = m_frame->height;
            if (!ensureSws(w, h, m_frame->format)) {
                emit error(QStringLiteral("Fallo al inicializar swscale"));
                return false;
            }
            out = QImage(w, h, QImage::Format_RGBA8888);
            uint8_t *dst[1] = { out.bits() };
            int dstStride[1] = { int(out.bytesPerLine()) };
            sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, h, dst, dstStride);

            const int64_t best = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp
                                     : m_frame->pts;
            ptsMs = best != AV_NOPTS_VALUE ? qint64(best * m_timeBaseSec * 1000.0) : m_lastPtsMs;
            av_frame_unref(m_frame);
            return true;
        }
        if (ret == AVERROR_EOF)
            return false;
        if (ret != AVERROR(EAGAIN)) {
            emit error(QStringLiteral("Error de decodificación"));
            return false;
        }

        // Necesita más paquetes.
        if (m_draining) {
            avcodec_send_packet(m_codec, nullptr);
            continue;
        }
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            m_draining = true;
            avcodec_send_packet(m_codec, nullptr); // vaciar
            continue;
        }
        if (m_pkt->stream_index == m_videoIndex)
            avcodec_send_packet(m_codec, m_pkt);
        av_packet_unref(m_pkt);
    }
}

void VideoDecoder::scheduleNext()
{
    if (!m_playing)
        return;

    QImage img;
    qint64 pts = 0;
    if (!decodeOneFrame(img, pts)) {
        m_playing = false;
        emit playbackFinished();
        return;
    }
    m_lastPtsMs = pts;

    const qint64 now = m_wall.elapsed();
    qint64 delay = (m_clockStartMs + pts) - now;
    if (delay < 0) delay = 0;
    if (delay > 1000) delay = 1000; // salvaguarda ante PTS raros

    QTimer::singleShot(int(delay), this, [this, img, pts]() {
        if (!m_playing)
            return;
        emit frameReady(img, pts);
        emit positionChanged(pts);
        scheduleNext();
    });
}

void VideoDecoder::play()
{
    if (!m_codec || m_playing)
        return;
    m_playing = true;
    m_clockStartMs = m_wall.elapsed() - m_lastPtsMs; // reanuda desde la última posición
    scheduleNext();
}

void VideoDecoder::pause()
{
    m_playing = false;
}

void VideoDecoder::stop()
{
    m_playing = false;
    seek(0);
}

void VideoDecoder::seek(qint64 ms)
{
    if (!m_fmt || m_videoIndex < 0)
        return;
    const int64_t ts = int64_t((ms / 1000.0) / m_timeBaseSec);
    av_seek_frame(m_fmt, m_videoIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_draining = false;

    QImage img;
    qint64 pts = 0;
    if (decodeOneFrame(img, pts)) {
        m_lastPtsMs = pts;
        m_clockStartMs = m_wall.elapsed() - pts;
        emit frameReady(img, pts);
        emit positionChanged(pts);
    }
}
