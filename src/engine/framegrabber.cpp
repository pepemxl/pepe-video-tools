#include "framegrabber.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

FrameGrabber::~FrameGrabber()
{
    close();
}

void FrameGrabber::close()
{
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_frame) av_frame_free(&m_frame);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_fmt) avformat_close_input(&m_fmt);
    m_videoIndex = -1;
    m_timeBaseSec = 0.0;
    m_durationMs = 0;
    m_eofSent = false;
}

bool FrameGrabber::open(const QString &path)
{
    close();
    const QByteArray utf8 = path.toUtf8();
    if (avformat_open_input(&m_fmt, utf8.constData(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) { close(); return false; }

    m_videoIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoIndex < 0) { close(); return false; }

    AVStream *stream = m_fmt->streams[m_videoIndex];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) { close(); return false; }

    m_codec = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(m_codec, stream->codecpar);
    m_codec->thread_count = 0;
    if (avcodec_open2(m_codec, decoder, nullptr) < 0) { close(); return false; }

    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    m_timeBaseSec = av_q2d(stream->time_base);

    if (m_fmt->duration > 0)
        m_durationMs = m_fmt->duration / (AV_TIME_BASE / 1000);
    else if (stream->duration > 0)
        m_durationMs = qint64(stream->duration * m_timeBaseSec * 1000.0);
    return true;
}

bool FrameGrabber::ensureSws(int w, int h, int srcFormat)
{
    if (m_sws)
        return true;
    m_sws = sws_getContext(w, h, static_cast<AVPixelFormat>(srcFormat),
                           w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    return m_sws != nullptr;
}

QImage FrameGrabber::decodeOne(qint64 &ptsMs)
{
    if (!m_codec)
        return {};
    for (;;) {
        int ret = avcodec_receive_frame(m_codec, m_frame);
        if (ret == 0) {
            const int w = m_frame->width, h = m_frame->height;
            if (!ensureSws(w, h, m_frame->format)) { av_frame_unref(m_frame); return {}; }
            QImage out(w, h, QImage::Format_RGBA8888);
            uint8_t *dst[1] = { out.bits() };
            int dstStride[1] = { int(out.bytesPerLine()) };
            sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, h, dst, dstStride);
            const int64_t best = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp : m_frame->pts;
            ptsMs = best != AV_NOPTS_VALUE ? qint64(best * m_timeBaseSec * 1000.0) : 0;
            av_frame_unref(m_frame);
            return out;
        }
        if (ret == AVERROR_EOF || ret != AVERROR(EAGAIN))
            return {};
        if (m_eofSent)
            return {};

        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            avcodec_send_packet(m_codec, nullptr); // vaciar
            m_eofSent = true;
            continue;
        }
        if (m_pkt->stream_index == m_videoIndex)
            avcodec_send_packet(m_codec, m_pkt);
        av_packet_unref(m_pkt);
    }
}

QImage FrameGrabber::frameAt(qint64 ms)
{
    if (!m_codec)
        return {};
    const int64_t ts = int64_t((ms / 1000.0) / m_timeBaseSec);
    av_seek_frame(m_fmt, m_videoIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_eofSent = false;

    QImage best;
    for (int i = 0; i < 240; ++i) { // límite de seguridad
        qint64 pts = -1;
        QImage img = decodeOne(pts);
        if (img.isNull())
            break;
        best = img;
        if (pts >= ms)
            break; // alcanzado el tiempo pedido
    }
    return best;
}
