#include "framegrabber.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
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
    if (m_swFrame) av_frame_free(&m_swFrame);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_fmt) avformat_close_input(&m_fmt);
    if (m_hwDev) av_buffer_unref(&m_hwDev);
    m_videoIndex = -1;
    m_swsFmt = -1;
    m_timeBaseSec = 0.0;
    m_durationMs = 0;
    m_eofSent = false;
    m_usingHw = false;
}

// El decoder ofrece una lista de formatos; con hw activo elegimos la superficie D3D11.
static AVPixelFormat pickHwFormat(AVCodecContext *, const AVPixelFormat *fmts)
{
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11)
            return AV_PIX_FMT_D3D11;
    return fmts[0]; // el decoder cae a su ruta software
}

bool FrameGrabber::open(const QString &path)
{
    // PVS_HWACCEL=0 desactiva la decodificación por hardware.
    bool ok = false;
    const int hwEnv = qEnvironmentVariableIntValue("PVS_HWACCEL", &ok);
    const bool wantHw = !ok || hwEnv != 0;
    if (wantHw && openInternal(path, true))
        return true;
    return openInternal(path, false);
}

bool FrameGrabber::openInternal(const QString &path, bool tryHw)
{
    close();
    m_path = path;
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

    if (tryHw) {
        // Solo si el decoder anuncia soporte de D3D11VA por contexto de dispositivo.
        bool supported = false;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(decoder, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) { supported = true; break; }
        }
        if (!supported
            || av_hwdevice_ctx_create(&m_hwDev, AV_HWDEVICE_TYPE_D3D11VA,
                                      nullptr, nullptr, 0) < 0) {
            close();
            return false;
        }
        m_codec->hw_device_ctx = av_buffer_ref(m_hwDev);
        m_codec->get_format = pickHwFormat;
        m_usingHw = true;
    }

    if (avcodec_open2(m_codec, decoder, nullptr) < 0) { close(); return false; }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    m_timeBaseSec = av_q2d(stream->time_base);

    // FrameCache: 96 MB por defecto; PVS_FRAMECACHE_MB lo ajusta (0 = desactivada).
    bool ok = false;
    const int mb = qEnvironmentVariableIntValue("PVS_FRAMECACHE_MB", &ok);
    m_cache.setMaxCost(qsizetype(ok ? qMax(0, mb) : 96) * 1024 * 1024);
    m_cache.clear();

    if (m_fmt->duration > 0)
        m_durationMs = m_fmt->duration / (AV_TIME_BASE / 1000);
    else if (stream->duration > 0)
        m_durationMs = qint64(stream->duration * m_timeBaseSec * 1000.0);
    return true;
}

bool FrameGrabber::ensureSws(int w, int h, int srcFormat)
{
    if (m_sws && m_swsFmt == srcFormat)
        return true;
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_sws = sws_getContext(w, h, static_cast<AVPixelFormat>(srcFormat),
                           w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_swsFmt = srcFormat;
    return m_sws != nullptr;
}

QImage FrameGrabber::decodeOne(qint64 &ptsMs)
{
    if (!m_codec)
        return {};
    for (;;) {
        int ret = avcodec_receive_frame(m_codec, m_frame);
        if (ret == 0) {
            // Con hw el fotograma vive en una superficie D3D11: cópialo a RAM (NV12).
            AVFrame *src = m_frame;
            if (m_frame->format == AV_PIX_FMT_D3D11) {
                av_frame_unref(m_swFrame);
                if (av_hwframe_transfer_data(m_swFrame, m_frame, 0) < 0) {
                    av_frame_unref(m_frame);
                    return {};
                }
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                src = m_swFrame;
            }
            const int w = src->width, h = src->height;
            if (!ensureSws(w, h, src->format)) { av_frame_unref(m_frame); return {}; }
            QImage out(w, h, QImage::Format_RGBA8888);
            uint8_t *dst[1] = { out.bits() };
            int dstStride[1] = { int(out.bytesPerLine()) };
            sws_scale(m_sws, src->data, src->linesize, 0, h, dst, dstStride);
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
    if (QImage *hit = m_cache.object(ms))
        return *hit;   // FrameCache: mismo tiempo pedido = sin seek ni decode
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
    // El decode por hardware falló con este archivo (driver/códec): reabre en
    // software y reintenta una vez. m_usingHw queda a false, sin recursión.
    if (best.isNull() && m_usingHw) {
        const QString p = m_path;
        if (openInternal(p, false))
            return frameAt(ms);
        return {};
    }
    if (!best.isNull() && m_cache.maxCost() > 0)
        m_cache.insert(ms, new QImage(best), qMax<qsizetype>(1, best.sizeInBytes()));
    return best;
}

// ============================ Auto-test ============================

#include "exporter.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

int runGrabSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_GRAB_SELFTEST"))
        return -1;

    int fails = 0;
    auto check = [&fails](bool ok, const char *what) {
        qInfo("[GRAB-SELFTEST] %-55s %s", what, ok ? "OK" : "FALLO");
        if (!ok) ++fails;
    };

    // 1) Genera un MP4 de prueba con el exportador: 2 s @ 24 fps, naranja → azul en 1 s.
    ExportJob job;
    job.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_grab_selftest.mp4"));
    job.width = 320; job.height = 180; job.fps = 24.0; job.durationUs = 2'000'000;
    for (int i = 0; i < 48; ++i) {
        TimelineModel::RenderClip rc;
        rc.trackIndex = 0; rc.kind = QStringLiteral("video");
        rc.fill = i < 24 ? QStringLiteral("#c06030") : QStringLiteral("#3060c0");
        rc.sourceUs = 0;
        job.frames.push_back(QVector<TimelineModel::RenderClip>{ rc });
    }
    QFile::remove(job.path);
    ExportWorker worker;
    bool encOk = false;
    QObject::connect(&worker, &ExportWorker::finished,
                     [&encOk](bool o, const QString &) { encOk = o; });
    worker.run(job);
    check(encOk && QFileInfo(job.path).size() > 1024, "genera el MP4 de prueba (exportador)");

    auto center = [](const QImage &f) { return f.pixelColor(f.width() / 2, f.height() / 2); };
    auto approx = [](const QColor &c, int r, int g, int b) {
        return qAbs(c.red() - r) < 28 && qAbs(c.green() - g) < 28 && qAbs(c.blue() - b) < 28;
    };

    // 2) Ruta por defecto (hardware si está disponible; si no, software transparente).
    QImage a0, a1;
    bool hwActive = false;
    {
        FrameGrabber g;
        check(g.open(job.path), "abre el MP4 (ruta por defecto)");
        hwActive = g.usingHw();
        a0 = g.frameAt(100);
        a1 = g.frameAt(1500);
        check(!a0.isNull() && a0.width() == 320 && a0.height() == 180,
              "fotograma 0.1 s decodificado (320x180)");
        check(!a0.isNull() && approx(center(a0), 0xc0, 0x60, 0x30), "0.1 s: color naranja");
        check(!a1.isNull() && approx(center(a1), 0x30, 0x60, 0xc0), "1.5 s: color azul");
    }
    qInfo("[GRAB-SELFTEST] decodificación por hardware (D3D11VA): %s",
          hwActive ? "activa" : "no disponible (software)");

    // 3) PVS_HWACCEL=0 fuerza software; los fotogramas deben coincidir (±8 por canal).
    qputenv("PVS_HWACCEL", "0");
    {
        FrameGrabber g;
        check(g.open(job.path) && !g.usingHw(), "PVS_HWACCEL=0 desactiva el hardware");
        const QImage b0 = g.frameAt(100), b1 = g.frameAt(1500);
        auto same = [](const QImage &a, const QImage &b) {
            if (a.isNull() || b.isNull() || a.size() != b.size())
                return false;
            int worst = 0;
            for (int y = 0; y < a.height(); y += 9)
                for (int x = 0; x < a.width(); x += 9) {
                    const QColor ca = a.pixelColor(x, y), cb = b.pixelColor(x, y);
                    worst = std::max({ worst, qAbs(ca.red() - cb.red()),
                                       qAbs(ca.green() - cb.green()),
                                       qAbs(ca.blue() - cb.blue()) });
                }
            return worst <= 8;
        };
        check(same(a0, b0) && same(a1, b1), "hardware y software producen lo mismo (±8)");
    }
    qunsetenv("PVS_HWACCEL");

    qInfo("[GRAB-SELFTEST] resultado: %s (%d fallos)", fails ? "FALLO" : "OK", fails);
    return fails ? 1 : 0;
}
