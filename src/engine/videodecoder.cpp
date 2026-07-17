#include "videodecoder.h"

#include <QTimer>
#include <cstring>

#include <d3d10.h>   // ID3D10Multithread (protección multihilo del immediate context)
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoDecoder::VideoDecoder(QObject *parent) : QObject(parent) {}

VideoDecoder::~VideoDecoder()
{
    closeFile();
    if (m_extDev) {
        static_cast<ID3D11Device *>(m_extDev)->Release();
        m_extDev = nullptr;
    }
}

void VideoDecoder::adoptDevice(void *d3dDevice)
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

    // Crítico: FFmpeg solo activa la protección multihilo del immediate context
    // cuando ÉL crea el dispositivo. Al compartir el de Qt, hay que activarla aquí
    // o el decode (este hilo) corrompe el contexto que usa el hilo de render.
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_ID3D10Multithread,
                                      reinterpret_cast<void **>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    } else {
        qWarning("[HW] el dispositivo no expone ID3D10Multithread: zero-copy desactivado");
        dev->Release();
        m_extDev = nullptr;
        return;
    }
    qInfo("[HW] dispositivo D3D11 del scene graph adoptado: decode zero-copy activo");

    // Reapertura silenciosa del medio ya abierto: la sesión pasa a decodificar en
    // el device de Qt. Si estaba reproduciendo, reanuda desde la misma posición
    // (un solo evento al adoptar el dispositivo, normalmente en el primer segundo).
    if (m_fmt && m_usingHw) {
        const bool wasPlaying = m_playing;
        const QString p = m_path;
        const qint64 pos = m_lastPtsMs;
        m_playing = false;
        QString err;
        if (openImpl(p, true, &err) || openImpl(p, false, &err)) {
            seek(pos);   // reemite el fotograma actual, ya por la ruta nueva
            if (wasPlaying)
                play();
        }
    }
}

void VideoDecoder::closeFile()
{
    m_playing = false;
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); }
    if (m_swFrame) { av_frame_free(&m_swFrame); }
    if (m_pkt) { av_packet_free(&m_pkt); }
    if (m_codec) { avcodec_free_context(&m_codec); }
    if (m_fmt) { avformat_close_input(&m_fmt); }
    if (m_hwDev) { av_buffer_unref(&m_hwDev); }
    m_videoIndex = -1;
    m_width = m_height = 0;
    m_swsFmt = -1;
    m_fps = 0.0;
    m_durationMs = 0;
    m_lastPtsMs = 0;
    m_draining = false;
    m_usingHw = false;
    m_zeroCopy = false;
    // m_extDev persiste entre archivos (se libera en el destructor).
}

// El decoder ofrece una lista de formatos; con hw activo elegimos la superficie D3D11.
static AVPixelFormat pickHwFormatDec(AVCodecContext *, const AVPixelFormat *fmts)
{
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11)
            return AV_PIX_FMT_D3D11;
    return fmts[0];
}

void VideoDecoder::openFile(const QString &path)
{
    bool ok = false;
    const int hwEnv = qEnvironmentVariableIntValue("PVS_HWACCEL", &ok);
    const bool wantHw = !ok || hwEnv != 0;

    QString err;
    if (wantHw && openImpl(path, true, &err)) {
        // abierto con hardware
    } else if (!openImpl(path, false, &err)) {
        emit error(err.isEmpty() ? QStringLiteral("No se pudo abrir: %1").arg(path) : err);
        emit opened(false, 0, 0, 0, 0.0);
        return;
    }

    emit opened(true, m_durationMs, m_width, m_height, m_fps);

    // Muestra el primer fotograma aunque esté en pausa.
    VideoFrame f;
    qint64 pts = 0;
    if (decodeOneFrame(f, pts)) {
        m_lastPtsMs = pts;
        emit frameReady(f, pts);
        emit positionChanged(pts);
    }
}

bool VideoDecoder::openImpl(const QString &path, bool tryHw, QString *err)
{
    closeFile();
    m_path = path;

    const QByteArray utf8 = path.toUtf8();
    if (avformat_open_input(&m_fmt, utf8.constData(), nullptr, nullptr) < 0) {
        if (err) *err = QStringLiteral("No se pudo abrir: %1").arg(path);
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        if (err) *err = QStringLiteral("Sin información de streams");
        closeFile();
        return false;
    }

    m_videoIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoIndex < 0) {
        if (err) *err = QStringLiteral("Sin stream de vídeo");
        closeFile();
        return false;
    }

    AVStream *stream = m_fmt->streams[m_videoIndex];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        if (err) *err = QStringLiteral("Códec no soportado");
        closeFile();
        return false;
    }

    m_codec = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(m_codec, stream->codecpar);
    m_codec->thread_count = 0; // auto (multihilo de decodificación)

    if (tryHw) {
        bool supported = false;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(decoder, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) { supported = true; break; }
        }
        if (!supported) {
            closeFile();
            return false;
        }
        if (m_extDev) {
            // Zero-copy: el contexto D3D11VA de FFmpeg se monta sobre el MISMO
            // dispositivo que usa el scene graph (av_hwdevice_ctx_init activa la
            // protección multihilo del device, que serializa el immediate context).
            m_hwDev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!m_hwDev) { closeFile(); return false; }
            auto *hc = reinterpret_cast<AVHWDeviceContext *>(m_hwDev->data);
            auto *dc = static_cast<AVD3D11VADeviceContext *>(hc->hwctx);
            dc->device = static_cast<ID3D11Device *>(m_extDev);
            dc->device->AddRef();   // el contexto suelta su referencia al destruirse
            if (av_hwdevice_ctx_init(m_hwDev) < 0) { closeFile(); return false; }
            m_zeroCopy = true;
        } else if (av_hwdevice_ctx_create(&m_hwDev, AV_HWDEVICE_TYPE_D3D11VA,
                                          nullptr, nullptr, 0) < 0) {
            closeFile();
            return false;
        }
        m_codec->hw_device_ctx = av_buffer_ref(m_hwDev);
        m_codec->get_format = pickHwFormatDec;
        m_usingHw = true;
    }

    if (avcodec_open2(m_codec, decoder, nullptr) < 0) {
        if (err) *err = QStringLiteral("No se pudo abrir el códec");
        closeFile();
        return false;
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
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
    return true;
}

bool VideoDecoder::ensureSws(int w, int h, int srcFormat)
{
    if (m_sws && m_swsFmt == srcFormat)
        return true;
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_sws = sws_getContext(w, h, static_cast<AVPixelFormat>(srcFormat),
                           w, h, AV_PIX_FMT_RGBA,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_swsFmt = srcFormat;
    return m_sws != nullptr;
}

// Zero-copy: copia (GPU→GPU) el slice NV12 del decodificador a una textura NV12
// propia con BIND_SHADER_RESOURCE, en el mismo dispositivo del scene graph. La
// textura viaja en el VideoFrame con Release() como deleter.
bool VideoDecoder::fillNativeFrame(VideoFrame &out)
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
    td.Width = sd.Width;          // tamaño alineado del decoder (out.width/height recorta)
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

bool VideoDecoder::decodeOneFrame(VideoFrame &out, qint64 &ptsMs)
{
    if (!m_codec)
        return false;

    static const bool gpuYuv = [] {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue("PVS_GPU_YUV", &ok);
        return !ok || v != 0;
    }();

    for (;;) {
        int ret = avcodec_receive_frame(m_codec, m_frame);
        if (ret == 0) {
            // Zero-copy: el slice D3D11 se copia GPU→GPU a una textura propia en el
            // dispositivo del scene graph; el fotograma nunca pasa por RAM.
            if (m_frame->format == AV_PIX_FMT_D3D11 && m_zeroCopy && gpuYuv) {
                out = VideoFrame();
                out.width = m_frame->width;
                out.height = m_frame->height;
                out.colorSpace = int(m_frame->colorspace);
                out.colorRange = int(m_frame->color_range);
                if (fillNativeFrame(out)) {
                    const int64_t bz = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                           ? m_frame->best_effort_timestamp
                                           : m_frame->pts;
                    ptsMs = bz != AV_NOPTS_VALUE ? qint64(bz * m_timeBaseSec * 1000.0)
                                                 : m_lastPtsMs;
                    av_frame_unref(m_frame);
                    return true;
                }
                // Si la copia falla, sigue por la ruta de transferencia a RAM.
            }
            // Con hw (sin zero-copy) el fotograma vive en una superficie D3D11:
            // cópialo a RAM (NV12).
            AVFrame *src = m_frame;
            if (m_frame->format == AV_PIX_FMT_D3D11) {
                av_frame_unref(m_swFrame);
                if (av_hwframe_transfer_data(m_swFrame, m_frame, 0) < 0) {
                    av_frame_unref(m_frame);
                    // Falla el decode por hardware: reabre en software y reintenta.
                    const QString p = m_path;
                    const qint64 resume = m_lastPtsMs;
                    if (openImpl(p, false, nullptr)) {
                        seekInternalOnly(resume);
                        return decodeOneFrame(out, ptsMs);
                    }
                    emit error(QStringLiteral("Error de decodificación (hardware)"));
                    return false;
                }
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                m_swFrame->colorspace = m_frame->colorspace;
                m_swFrame->color_range = m_frame->color_range;
                src = m_swFrame;
            }

            const int w = src->width;
            const int h = src->height;
            out = VideoFrame();
            out.width = w;
            out.height = h;
            out.colorSpace = int(src->colorspace);
            out.colorRange = int(src->color_range);

            const int cw = w / 2, ch = h / 2;
            if (gpuYuv && src->format == AV_PIX_FMT_YUV420P && (w % 2) == 0 && (h % 2) == 0) {
                out.y = packPlane(src->data[0], src->linesize[0], w, h);
                out.u = packPlane(src->data[1], src->linesize[1], cw, ch);
                out.v = packPlane(src->data[2], src->linesize[2], cw, ch);
            } else if (gpuYuv && src->format == AV_PIX_FMT_NV12 && (w % 2) == 0 && (h % 2) == 0) {
                out.y = packPlane(src->data[0], src->linesize[0], w, h);
                // NV12: croma intercalado UVUV… → desintercala a planos U y V.
                out.u = QByteArray(cw * ch, Qt::Uninitialized);
                out.v = QByteArray(cw * ch, Qt::Uninitialized);
                for (int r = 0; r < ch; ++r) {
                    const uint8_t *uv = src->data[1] + qsizetype(r) * src->linesize[1];
                    char *du = out.u.data() + qsizetype(r) * cw;
                    char *dv = out.v.data() + qsizetype(r) * cw;
                    for (int x = 0; x < cw; ++x) { du[x] = char(uv[2 * x]); dv[x] = char(uv[2 * x + 1]); }
                }
            } else {
                // Formato exótico (o PVS_GPU_YUV=0): ruta clásica por swscale a RGBA.
                if (!ensureSws(w, h, src->format)) {
                    emit error(QStringLiteral("Fallo al inicializar swscale"));
                    av_frame_unref(m_frame);
                    return false;
                }
                out.rgba = QImage(w, h, QImage::Format_RGBA8888);
                uint8_t *dst[1] = { out.rgba.bits() };
                int dstStride[1] = { int(out.rgba.bytesPerLine()) };
                sws_scale(m_sws, src->data, src->linesize, 0, h, dst, dstStride);
            }

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
            // Con hardware, un error duro puede ser del driver: reintenta en software.
            if (m_usingHw) {
                const QString p = m_path;
                const qint64 resume = m_lastPtsMs;
                if (openImpl(p, false, nullptr)) {
                    seekInternalOnly(resume);
                    return decodeOneFrame(out, ptsMs);
                }
            }
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

    VideoFrame f;
    qint64 pts = 0;
    if (!decodeOneFrame(f, pts)) {
        m_playing = false;
        emit playbackFinished();
        return;
    }
    m_lastPtsMs = pts;

    const qint64 now = m_wall.elapsed();
    qint64 delay = (m_clockStartMs + pts) - now;
    if (delay < 0) delay = 0;
    if (delay > 1000) delay = 1000; // salvaguarda ante PTS raros

    QTimer::singleShot(int(delay), this, [this, f, pts]() {
        if (!m_playing)
            return;
        emit frameReady(f, pts);
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

// Reposiciona el demuxer/decoder sin decodificar ni emitir (para el fallback hw→sw).
void VideoDecoder::seekInternalOnly(qint64 ms)
{
    if (!m_fmt || m_videoIndex < 0)
        return;
    const int64_t ts = int64_t((ms / 1000.0) / m_timeBaseSec);
    av_seek_frame(m_fmt, m_videoIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_draining = false;
    m_lastPtsMs = ms;
    m_clockStartMs = m_wall.elapsed() - ms;
}

void VideoDecoder::seek(qint64 ms)
{
    if (!m_fmt || m_videoIndex < 0)
        return;
    const int64_t ts = int64_t((ms / 1000.0) / m_timeBaseSec);
    av_seek_frame(m_fmt, m_videoIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_draining = false;

    VideoFrame f;
    qint64 pts = 0;
    // Avanza hasta el fotograma pedido (el seek cae en el keyframe anterior).
    for (int i = 0; i < 240; ++i) {
        if (!decodeOneFrame(f, pts))
            break;
        if (pts >= ms || !f.isValid())
            break;
    }
    if (f.isValid()) {
        m_lastPtsMs = pts;
        m_clockStartMs = m_wall.elapsed() - pts;
        emit frameReady(f, pts);
        emit positionChanged(pts);
    }
}
