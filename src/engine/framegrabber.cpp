#include "framegrabber.h"

#include <QDebug>
#include <QElapsedTimer>

#include <cstring>

#include <d3d10.h>   // ID3D10Multithread
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

static bool gpuYuvEnabled()
{
    static const bool v = [] {
        bool ok = false;
        const int e = qEnvironmentVariableIntValue("PVS_GPU_YUV", &ok);
        return !ok || e != 0;
    }();
    return v;
}

FrameGrabber::~FrameGrabber()
{
    close();
    if (m_extDev) {
        static_cast<ID3D11Device *>(m_extDev)->Release();
        m_extDev = nullptr;
    }
}

void FrameGrabber::adoptDevice(void *d3dDevice)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("PVS_ZEROCOPY", &ok);
    if ((ok && v == 0) || !d3dDevice || m_extDev == d3dDevice)
        return;
    if (m_extDev)
        static_cast<ID3D11Device *>(m_extDev)->Release();
    m_extDev = d3dDevice;
    auto *dev = static_cast<ID3D11Device *>(m_extDev);
    dev->AddRef();
    // Protección multihilo del immediate context (compartido con el hilo de render).
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D10Multithread,
                                      reinterpret_cast<void **>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    } else {
        dev->Release();
        m_extDev = nullptr;
    }
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
    m_zeroCopy = false;
    // m_extDev persiste entre archivos (se libera en el destructor).
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
        if (!supported) {
            close();
            return false;
        }
        if (m_extDev) {
            // Zero-copy: contexto D3D11VA sobre el MISMO dispositivo del scene graph.
            m_hwDev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!m_hwDev) { close(); return false; }
            auto *hc = reinterpret_cast<AVHWDeviceContext *>(m_hwDev->data);
            auto *dc = static_cast<AVD3D11VADeviceContext *>(hc->hwctx);
            dc->device = static_cast<ID3D11Device *>(m_extDev);
            dc->device->AddRef();   // el contexto suelta su referencia al destruirse
            if (av_hwdevice_ctx_init(m_hwDev) < 0) { close(); return false; }
            m_zeroCopy = true;
        } else if (av_hwdevice_ctx_create(&m_hwDev, AV_HWDEVICE_TYPE_D3D11VA,
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
    m_lastPtsMs = -1;
    m_last = VideoFrame();

    // Duración de un fotograma del origen (para cuantizar las peticiones a su
    // rejilla): avg_frame_rate, con r_frame_rate de reserva.
    m_frameDurMs = 0.0;
    AVRational fr = stream->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = stream->r_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        const double fps = av_q2d(fr);
        if (fps > 1.0 && fps <= 480.0)
            m_frameDurMs = 1000.0 / fps;
    }

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

// Zero-copy: copia (GPU→GPU) el slice NV12 del decodificador a una textura NV12
// propia con BIND_SHADER_RESOURCE, en el mismo dispositivo del scene graph.
bool FrameGrabber::fillNative(VideoFrame &out)
{
    auto *src = reinterpret_cast<ID3D11Texture2D *>(m_frame->data[0]);
    const UINT slice = UINT(reinterpret_cast<intptr_t>(m_frame->data[1]));
    if (!src || !m_extDev)
        return false;
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    if (sd.Format != DXGI_FORMAT_NV12)
        return false;

    auto *dev = static_cast<ID3D11Device *>(m_extDev);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sd.Width;
    td.Height = sd.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *dst = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &dst)) || !dst)
        return false;

    ID3D11DeviceContext *ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, slice, nullptr);
    ctx->Release();

    out.native = std::shared_ptr<void>(static_cast<void *>(dst), [](void *p) {
        if (p) static_cast<ID3D11Texture2D *>(p)->Release();
    });
    out.texW = int(td.Width);
    out.texH = int(td.Height);
    return true;
}

// Copia un plano a un QByteArray empaquetado (stride = ancho), fila a fila.
static QByteArray packPlane(const uint8_t *src, int stride, int w, int h)
{
    QByteArray out(w * h, Qt::Uninitialized);
    char *dst = out.data();
    for (int r = 0; r < h; ++r)
        std::memcpy(dst + qsizetype(r) * w, src + qsizetype(r) * stride, w);
    return out;
}

bool FrameGrabber::decodeOne(VideoFrame &out, qint64 &ptsMs, bool forceCpu)
{
    if (!m_codec)
        return {};
    for (;;) {
        int ret = avcodec_receive_frame(m_codec, m_frame);
        if (ret == 0) {
            out = VideoFrame();
            out.width = m_frame->width;
            out.height = m_frame->height;
            out.colorSpace = int(m_frame->colorspace);
            out.colorRange = int(m_frame->color_range);

            // Zero-copy: el slice se queda en el GPU (textura NV12 propia).
            if (m_frame->format == AV_PIX_FMT_D3D11 && m_zeroCopy && !forceCpu
                && gpuYuvEnabled() && fillNative(out)) {
                const int64_t b = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                      ? m_frame->best_effort_timestamp : m_frame->pts;
                ptsMs = b != AV_NOPTS_VALUE ? qint64(b * m_timeBaseSec * 1000.0) : 0;
                av_frame_unref(m_frame);
                return true;
            }

            // Con hw el fotograma vive en una superficie D3D11: cópialo a RAM (NV12).
            AVFrame *src = m_frame;
            if (m_frame->format == AV_PIX_FMT_D3D11) {
                av_frame_unref(m_swFrame);
                if (av_hwframe_transfer_data(m_swFrame, m_frame, 0) < 0) {
                    av_frame_unref(m_frame);
                    return false;
                }
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                src = m_swFrame;
            }
            const int w = src->width, h = src->height;
            const int cw = w / 2, ch = h / 2;
            if (gpuYuvEnabled() && src->format == AV_PIX_FMT_YUV420P
                && (w % 2) == 0 && (h % 2) == 0) {
                out.y = packPlane(src->data[0], src->linesize[0], w, h);
                out.u = packPlane(src->data[1], src->linesize[1], cw, ch);
                out.v = packPlane(src->data[2], src->linesize[2], cw, ch);
            } else if (gpuYuvEnabled() && src->format == AV_PIX_FMT_NV12
                       && (w % 2) == 0 && (h % 2) == 0) {
                out.y = packPlane(src->data[0], src->linesize[0], w, h);
                out.u = QByteArray(cw * ch, Qt::Uninitialized);
                out.v = QByteArray(cw * ch, Qt::Uninitialized);
                for (int r = 0; r < ch; ++r) {
                    const uint8_t *uv = src->data[1] + qsizetype(r) * src->linesize[1];
                    char *du = out.u.data() + qsizetype(r) * cw;
                    char *dv = out.v.data() + qsizetype(r) * cw;
                    for (int x = 0; x < cw; ++x) { du[x] = char(uv[2 * x]); dv[x] = char(uv[2 * x + 1]); }
                }
            } else {
                if (!ensureSws(w, h, src->format)) { av_frame_unref(m_frame); return false; }
                out.rgba = QImage(w, h, QImage::Format_RGBA8888);
                uint8_t *dst[1] = { out.rgba.bits() };
                int dstStride[1] = { int(out.rgba.bytesPerLine()) };
                sws_scale(m_sws, src->data, src->linesize, 0, h, dst, dstStride);
            }
            const int64_t best = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp : m_frame->pts;
            ptsMs = best != AV_NOPTS_VALUE ? qint64(best * m_timeBaseSec * 1000.0) : 0;
            av_frame_unref(m_frame);
            return true;
        }
        if (ret == AVERROR_EOF || ret != AVERROR(EAGAIN))
            return false;
        if (m_eofSent)
            return false;

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

// Planos I420 empaquetados → RGBA con swscale (para el compositor CPU/exportador).
QImage FrameGrabber::planesToRgba(const VideoFrame &vf)
{
    if (!vf.hasYuv())
        return {};
    SwsContext *sws = sws_getContext(vf.width, vf.height, AV_PIX_FMT_YUV420P,
                                     vf.width, vf.height, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
        return {};
    QImage out(vf.width, vf.height, QImage::Format_RGBA8888);
    const uint8_t *src[3] = { reinterpret_cast<const uint8_t *>(vf.y.constData()),
                              reinterpret_cast<const uint8_t *>(vf.u.constData()),
                              reinterpret_cast<const uint8_t *>(vf.v.constData()) };
    const int srcStride[3] = { vf.width, vf.width / 2, vf.width / 2 };
    uint8_t *dst[1] = { out.bits() };
    int dstStride[1] = { int(out.bytesPerLine()) };
    sws_scale(sws, src, srcStride, 0, vf.height, dst, dstStride);
    sws_freeContext(sws);
    return out;
}

static qsizetype frameCost(const VideoFrame &vf)
{
    qsizetype c = vf.y.size() + vf.u.size() + vf.v.size() + vf.rgba.sizeInBytes();
    if (vf.native)
        c += qsizetype(vf.texW) * vf.texH * 3 / 2;   // estimación de la textura NV12
    return qMax<qsizetype>(1, c);
}

// Cuantiza un tiempo a la rejilla de fotogramas del origen: todas las
// peticiones que caen dentro del mismo fotograma comparten clave (caché) y
// objetivo de decode. Sin fps conocidos devuelve el ms tal cual.
qint64 FrameGrabber::quantizeMs(qint64 ms) const
{
    if (m_frameDurMs <= 0.0 || ms <= 0)
        return qMax<qint64>(0, ms);
    const qint64 idx = qint64(ms / m_frameDurMs);
    return qint64(idx * m_frameDurMs + 0.5);
}

VideoFrame FrameGrabber::frameVfAt(qint64 ms, bool wantCpuPixels)
{
    if (!m_codec)
        return {};
    ms = quantizeMs(ms);
    if (VideoFrame *hit = m_cache.object(ms)) {
        if (!wantCpuPixels || hit->hasYuv() || !hit->rgba.isNull())
            return *hit;
        // Solo hay textura nativa y se piden píxeles CPU: re-decodifica abajo.
    }
    // El último fotograma decodificado aún CUBRE el tiempo pedido (la rejilla
    // cuantizada y los pts reales del archivo no coinciden exactamente): sirve
    // tal cual, sin volver a decodificar — y sobre todo sin seek hacia atrás.
    if (m_last.isValid() && m_lastPtsMs >= 0 && ms >= m_lastPtsMs
        && m_frameDurMs > 0.0 && ms < m_lastPtsMs + m_frameDurMs
        && (!wantCpuPixels || m_last.hasYuv() || !m_last.rgba.isNull())) {
        if (m_cache.maxCost() > 0)
            m_cache.insert(ms, new VideoFrame(m_last), frameCost(m_last));
        return m_last;
    }

    // Reproducción hacia adelante: si el tiempo pedido está POR DELANTE de la
    // posición del decodificador y cerca, continúa el decode secuencial (como
    // un reproductor). El seek+flush al keyframe anterior — que obliga a
    // re-decodificar todo el GOP — queda solo para saltos y retrocesos.
    constexpr qint64 kSeqWindowMs = 2000;
    const bool sequential = m_lastPtsMs >= 0 && !m_eofSent && ms > m_lastPtsMs
                            && (ms - m_lastPtsMs) <= kSeqWindowMs;
    if (!sequential) {
        const int64_t ts = int64_t((ms / 1000.0) / m_timeBaseSec);
        av_seek_frame(m_fmt, m_videoIndex, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m_codec);
        m_eofSent = false;
        m_lastPtsMs = -1;
    }

    QElapsedTimer dbgTimer;
    dbgTimer.start();

    VideoFrame best;
    qint64 bestPts = -1;
    int decoded = 0;
    for (int i = 0; i < 600; ++i) { // límite de seguridad
        qint64 pts = -1;
        VideoFrame vf;
        if (!decodeOne(vf, pts, wantCpuPixels))
            break;
        best = vf;
        bestPts = pts;
        ++decoded;
        // Para: este fotograma CUBRE el tiempo pedido ([pts, pts+dur) ∋ ms).
        // Con `pts >= ms` a secas, un pts truncado 1 ms por debajo del pedido
        // haría decodificar (y mostrar) el fotograma SIGUIENTE.
        if (m_frameDurMs > 0.0 ? (pts + m_frameDurMs > ms) : (pts >= ms))
            break;
    }
    if (bestPts >= 0) {
        m_lastPtsMs = bestPts;
        m_last = best;
    }

    static const bool grabDebug = qEnvironmentVariableIntValue("PVS_GRAB_DEBUG") == 1;
    if (grabDebug)
        qInfo("[GRAB] ms=%lld %s decodificados=%d pts=%lld en %lld ms",
              ms, sequential ? "seq" : "SEEK", decoded, bestPts, dbgTimer.elapsed());
    // El decode por hardware falló con este archivo (driver/códec): reabre en
    // software y reintenta una vez.
    if (!best.isValid() && m_usingHw) {
        const QString p = m_path;
        if (openInternal(p, false))
            return frameVfAt(ms, wantCpuPixels);
        return {};
    }
    if (best.isValid() && m_cache.maxCost() > 0)
        m_cache.insert(ms, new VideoFrame(best), frameCost(best));
    return best;
}

QImage FrameGrabber::frameAt(qint64 ms)
{
    ms = quantizeMs(ms);   // misma clave de caché que frameVfAt
    VideoFrame vf = frameVfAt(ms, true);
    if (!vf.rgba.isNull())
        return vf.rgba;
    if (!vf.hasYuv())
        return {};
    QImage rgba = planesToRgba(vf);
    // Guarda la conversión en el caché para las siguientes peticiones del mismo ms.
    if (!rgba.isNull())
        if (VideoFrame *hit = m_cache.object(ms))
            hit->rgba = rgba;
    return rgba;
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
    const QString mp4 = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_grab_selftest.mp4"));
    check(pvsWriteColorTestMp4(mp4), "genera el MP4 de prueba (exportador)");

    auto center = [](const QImage &f) { return f.pixelColor(f.width() / 2, f.height() / 2); };
    auto approx = [](const QColor &c, int r, int g, int b) {
        return qAbs(c.red() - r) < 28 && qAbs(c.green() - g) < 28 && qAbs(c.blue() - b) < 28;
    };

    // 2) Ruta por defecto (hardware si está disponible; si no, software transparente).
    QImage a0, a1;
    bool hwActive = false;
    {
        FrameGrabber g;
        check(g.open(mp4), "abre el MP4 (ruta por defecto)");
        hwActive = g.usingHw();
        a0 = g.frameAt(100);
        a1 = g.frameAt(1500);
        check(!a0.isNull() && a0.width() == 320 && a0.height() == 180,
              "fotograma 0.1 s decodificado (320x180)");
        check(!a0.isNull() && approx(center(a0), 0xc0, 0x60, 0x30), "0.1 s: color naranja");
        check(!a1.isNull() && approx(center(a1), 0x30, 0x60, 0xc0), "1.5 s: color azul");
        // La ruta VideoFrame entrega planos I420 (o RGBA) coherentes con frameAt.
        const VideoFrame vf = g.frameVfAt(100);
        check(vf.isValid() && (vf.hasYuv() || !vf.rgba.isNull()),
              "frameVfAt entrega planos YUV o RGBA");
    }
    qInfo("[GRAB-SELFTEST] decodificación por hardware (D3D11VA): %s",
          hwActive ? "activa" : "no disponible (software)");

    // 3) PVS_HWACCEL=0 fuerza software; los fotogramas deben coincidir (±8 por canal).
    qputenv("PVS_HWACCEL", "0");
    {
        FrameGrabber g;
        check(g.open(mp4) && !g.usingHw(), "PVS_HWACCEL=0 desactiva el hardware");
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
