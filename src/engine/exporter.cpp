#include "exporter.h"
#include "compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <new>
#include <string>

#include "audioengine.h" // AudioMixClip + AudioPlayer (horneado de la mezcla maestra)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#  include <windows.h>
#  include <commdlg.h>
#endif

// ============================ ExportWorker ============================

namespace {

// Drena los paquetes de un encoder y los escribe (intercalados) en el muxer.
// Devuelve false ante un error de escritura/codificación.
bool drainEncoder(AVFormatContext *oc, AVCodecContext *enc, AVStream *st, AVPacket *pkt)
{
    for (;;) {
        int ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return true;
        if (ret < 0)
            return false;
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = st->index;
        ret = av_interleaved_write_frame(oc, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            return false;
    }
}

// Tabla de formatos de salida: id · etiqueta · extensión · encoder de vídeo · pix_fmt ·
// perfil (ProRes/DNxHR) · usa bitrate · encoder de audio · sample_fmt de audio.
struct FmtDesc {
    const char *id, *label, *ext, *venc;
    AVPixelFormat pix;
    const char *vprofile;   // nullptr, o perfil de calidad (prores/dnxhr)
    bool useBitrate;        // true: H.264/H.265/VP9; false: calidad por perfil
    const char *aenc;
    AVSampleFormat asfmt;
    bool hwAmf = false;     // true: encoder por hardware AMD (AMF); opciones distintas + fallback
    bool streamCopy = false; // true: copia directa (remux sin recodificar); no usa encoders
};
const FmtDesc kFormats[] = {
    { "h264",     "H.264 · MP4",        "mp4",  "libx264",    AV_PIX_FMT_YUV420P,     nullptr,     true,  "aac",       AV_SAMPLE_FMT_FLTP },
    { "h265",     "H.265/HEVC · MP4",   "mp4",  "libx265",    AV_PIX_FMT_YUV420P,     nullptr,     true,  "aac",       AV_SAMPLE_FMT_FLTP },
    { "h264_amf", "H.264 · AMD GPU · MP4", "mp4", "h264_amf", AV_PIX_FMT_NV12,        nullptr,     true,  "aac",       AV_SAMPLE_FMT_FLTP, true },
    { "h265_amf", "H.265 · AMD GPU · MP4", "mp4", "hevc_amf", AV_PIX_FMT_NV12,        nullptr,     true,  "aac",       AV_SAMPLE_FMT_FLTP, true },
    { "prores_lt","ProRes 422 LT · MOV","mov",  "prores_ks",  AV_PIX_FMT_YUV422P10LE, "lt",        false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "prores",   "ProRes 422 · MOV",   "mov",  "prores_ks",  AV_PIX_FMT_YUV422P10LE, "standard",  false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "prores_hq","ProRes 422 HQ · MOV","mov",  "prores_ks",  AV_PIX_FMT_YUV422P10LE, "hq",        false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "prores4444","ProRes 4444 · MOV", "mov",  "prores_ks",  AV_PIX_FMT_YUV444P10LE, "4444",      false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "dnxhr_sq", "DNxHR SQ · MOV",     "mov",  "dnxhd",      AV_PIX_FMT_YUV422P,     "dnxhr_sq",  false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "dnxhr",    "DNxHR HQ · MOV",     "mov",  "dnxhd",      AV_PIX_FMT_YUV422P,     "dnxhr_hq",  false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "vp9",      "VP9 · WebM",         "webm", "libvpx-vp9", AV_PIX_FMT_YUV420P,     nullptr,     true,  "libopus",   AV_SAMPLE_FMT_S16 },
    { "copy",     "Copia directa · sin recodificar · MP4", "mp4", "", AV_PIX_FMT_NONE, nullptr, false, "", AV_SAMPLE_FMT_NONE, false, true },
};
const FmtDesc &fmtById(const QString &id)
{
    for (const auto &f : kFormats)
        if (id == QLatin1String(f.id)) return f;
    return kFormats[0];   // por defecto H.264
}

// ¿El encoder por hardware AMD (AMF) está realmente utilizable en esta máquina?
// avcodec_find_encoder_by_name devuelve el encoder si está compilado en FFmpeg, aunque
// no haya GPU AMD ni runtime (amfrt64.dll). Para no ofrecer en la UI una opción que
// fallaría, se abre una vez un contexto AMF mínimo: si abre, hay GPU+runtime AMD.
bool amfUsable()
{
    static const bool ok = [] {
        const AVCodec *c = avcodec_find_encoder_by_name("h264_amf");
        if (!c) return false;
        AVCodecContext *ctx = avcodec_alloc_context3(c);
        if (!ctx) return false;
        ctx->width = 640; ctx->height = 480;
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        ctx->time_base = AVRational{ 1, 30 };
        ctx->framerate = AVRational{ 30, 1 };
        ctx->bit_rate = 2'000'000;
        const bool good = (avcodec_open2(ctx, c, nullptr) == 0);
        avcodec_free_context(&ctx);
        return good;
    }();
    return ok;
}

// Copia directa (stream copy): remultiplexa las pistas de vídeo y audio del origen al
// MP4 de salida SIN recodificar, recortando al rango [copySrcStartUs, copySrcEndUs].
// El corte arranca en el keyframe <= inicio (limitación inherente de copiar sin
// recodificar). Reajusta los timestamps para que la salida empiece en 0.
bool remuxStreamCopy(const ExportJob &job, QString &err)
{
    const AVRational usTb{ 1, 1000000 };
    AVFormatContext *in = nullptr;
    const QByteArray inPath = QFile::encodeName(job.copySrcPath);
    if (avformat_open_input(&in, inPath.constData(), nullptr, nullptr) < 0) {
        err = QStringLiteral("No se pudo abrir el origen para copiar."); return false;
    }
    if (avformat_find_stream_info(in, nullptr) < 0) {
        avformat_close_input(&in);
        err = QStringLiteral("No se pudo leer la información de streams del origen."); return false;
    }

    AVFormatContext *out = nullptr;
    const QByteArray outPath = QFile::encodeName(job.path);
    avformat_alloc_output_context2(&out, nullptr, nullptr, outPath.constData());
    if (!out) {
        avformat_close_input(&in);
        err = QStringLiteral("No se pudo crear el contenedor de salida."); return false;
    }

    // Mapea cada stream de vídeo/audio del origen a un stream de salida (copia de params).
    QVector<int> streamMap(int(in->nb_streams), -1);
    int outIdx = 0;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVMediaType t = in->streams[i]->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) continue;
        AVStream *os = avformat_new_stream(out, nullptr);
        if (!os) { err = QStringLiteral("No se pudo crear el stream de salida."); goto failOut; }
        if (avcodec_parameters_copy(os->codecpar, in->streams[i]->codecpar) < 0) {
            err = QStringLiteral("No se pudieron copiar los parámetros del códec."); goto failOut;
        }
        os->codecpar->codec_tag = 0;   // deja que el muxer elija la etiqueta del contenedor
        streamMap[int(i)] = outIdx++;
    }
    if (outIdx == 0) { err = QStringLiteral("El origen no tiene pistas de vídeo/audio."); goto failOut; }

    if (!(out->oformat->flags & AVFMT_NOFILE)
        && avio_open(&out->pb, outPath.constData(), AVIO_FLAG_WRITE) < 0) {
        err = QStringLiteral("No se pudo abrir el archivo de salida."); goto failOut;
    }
    if (avformat_write_header(out, nullptr) < 0) {
        err = QStringLiteral("No se pudo escribir la cabecera del MP4."); goto failClose;
    }

    {
        // Sitúa el origen en el keyframe <= inicio del rango.
        const int64_t startTs = av_rescale_q(job.copySrcStartUs, usTb, AV_TIME_BASE_Q);
        av_seek_frame(in, -1, startTs, AVSEEK_FLAG_BACKWARD);

        int64_t offsetUs = AV_NOPTS_VALUE;   // origen de tiempos (primer paquete) → 0 en salida
        bool videoEnded = false;
        AVPacket *pkt = av_packet_alloc();
        while (av_read_frame(in, pkt) >= 0) {
            const int si = pkt->stream_index;
            if (si >= streamMap.size() || streamMap[si] < 0) { av_packet_unref(pkt); continue; }
            AVStream *is = in->streams[si];
            const bool isVideo = (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
            const int64_t ref = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            const int64_t srcUs = av_rescale_q(ref, is->time_base, usTb);
            // Fin del rango: al pasar el último fotograma de vídeo, deja de leer.
            if (srcUs > job.copySrcEndUs) {
                if (isVideo) { av_packet_unref(pkt); videoEnded = true; break; }
                av_packet_unref(pkt); continue;   // audio sobrante se descarta
            }
            if (offsetUs == AV_NOPTS_VALUE) offsetUs = srcUs;   // el primero (más temprano) marca el 0
            const int64_t offTb = av_rescale_q(offsetUs, usTb, is->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = qMax<int64_t>(0, pkt->pts - offTb);
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts = qMax<int64_t>(0, pkt->dts - offTb);
            AVStream *os = out->streams[streamMap[si]];
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            pkt->stream_index = streamMap[si];
            pkt->pos = -1;
            av_interleaved_write_frame(out, pkt);   // toma posesión y libera el paquete
        }
        av_packet_free(&pkt);
        (void)videoEnded;
        av_write_trailer(out);
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    return true;

failClose:
    if (!(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
failOut:
    avformat_free_context(out);
    avformat_close_input(&in);
    return false;
}

} // namespace

void ExportWorker::run(const ExportJob &job)
{
    auto fail = [this](const QString &m) {
        qCritical("[export] FALLO: %s", qUtf8Printable(m));
        emit finished(false, m);
    };

    // Copia directa (stream copy): remux sin recodificar, ruta aparte del pipeline de
    // composición/encoders. No usa fotogramas ni audio horneado.
    if (job.streamCopy) {
        qInfo("[export] copia directa: %s [%.1f-%.1f s] -> %s",
              qUtf8Printable(job.copySrcPath), job.copySrcStartUs / 1e6,
              job.copySrcEndUs / 1e6, qUtf8Printable(job.path));
        QString err;
        if (remuxStreamCopy(job, err)) {
            qInfo("[export] copia directa completada: %s (%lld bytes)",
                  qUtf8Printable(job.path), qint64(QFileInfo(job.path).size()));
            emit finished(true, QStringLiteral("Copia directa completada."));
        } else {
            fail(err);
        }
        return;
    }

    const int W = job.width, H = job.height;
    const int frameCount = job.streamFrames ? job.frameCount : int(job.frames.size());
    qInfo("[export] run: %dx%d @%.3f fps, %d fotogramas (%s), formato=%s, audio=%d B -> %s",
          W, H, job.fps, frameCount, job.streamFrames ? "streaming" : "materializados",
          qUtf8Printable(job.format), int(job.audioS16.size()), qUtf8Printable(job.path));
    if (frameCount == 0) { fail(QStringLiteral("Nada que exportar (sin fotogramas).")); return; }

    // Recorte de salida (reencuadre): rectángulo a conservar del fotograma compuesto
    // (W×H) que luego se escala de vuelta a W×H. cropX/Y/W/H en píxeles de la imagen
    // compuesta; el swscale lee ese sub-rect y escala al lienzo de salida.
    const double clL = qBound(0.0, job.cropLeft, 0.49), clR = qBound(0.0, job.cropRight, 0.49);
    const double clT = qBound(0.0, job.cropTop, 0.49),  clB = qBound(0.0, job.cropBottom, 0.49);
    const int cropX = int(std::lround(clL * W));
    const int cropY = int(std::lround(clT * H));
    const int cropW = qMax(2, W - cropX - int(std::lround(clR * W)));
    const int cropH = qMax(2, H - cropY - int(std::lround(clB * H)));
    const bool cropping = (cropX > 0 || cropY > 0 || cropW < W || cropH < H);
    if (cropping)
        qInfo("[export] recorte de salida: sub-rect %dx%d en (%d,%d) escalado a %dx%d",
              cropW, cropH, cropX, cropY, W, H);

    // Resolución de un fotograma (streaming o materializado), reutilizada por ambas pasadas.
    auto frameAt = [&](int i) -> QVector<TimelineModel::RenderClip> {
        return job.streamFrames
            ? TimelineModel::clipsAtSnapshot(job.snapshot, job.startUs + qint64((double(i) / job.fps) * 1e6))
            : job.frames[i];
    };

    // --- Compositor propio de este hilo (posee sus FrameGrabber) ---
    CompositorWorker comp(QSize(W, H));

    // --- Muxer MP4 ---
    AVFormatContext *oc = nullptr;
    const QByteArray pathUtf8 = QFile::encodeName(job.path);
    avformat_alloc_output_context2(&oc, nullptr, nullptr, pathUtf8.constData());
    if (!oc) { fail(QStringLiteral("No se pudo crear el contenedor de salida.")); return; }

    // --- Encoder de vídeo según el formato elegido ---
    const FmtDesc &fmt = fmtById(job.format);
    const AVCodec *vcodec = avcodec_find_encoder_by_name(fmt.venc);
    if (!vcodec) {
        avformat_free_context(oc);
        fail(QStringLiteral("Encoder de vídeo no disponible: %1.").arg(QLatin1String(fmt.venc))); return;
    }

    // 2 pasadas: solo en modo bitrate con H.264/H.265. La 1.ª analiza y escribe stats.
    const bool twoPass = job.twoPass && fmt.useBitrate && !job.useCrf
                         && (qstrcmp(fmt.venc, "libx264") == 0 || qstrcmp(fmt.venc, "libx265") == 0);
    const QByteArray statsFile = QFile::encodeName(job.path + QStringLiteral(".pass.log"));
    if (twoPass) {
        qInfo("[export] 2-pass: pasada 1 (análisis)…");
        AVCodecContext *p1 = avcodec_alloc_context3(vcodec);
        p1->width = W; p1->height = H; p1->pix_fmt = fmt.pix;
        p1->time_base = av_d2q(1.0 / job.fps, 1000000);
        p1->framerate = av_d2q(job.fps, 1000000);
        p1->gop_size = 12; p1->bit_rate = job.videoBitrate;
        p1->flags |= AV_CODEC_FLAG_PASS1;
        av_opt_set(p1->priv_data, "preset", "medium", 0);
        if (!job.videoProfile.isEmpty())
            av_opt_set(p1->priv_data, "profile", job.videoProfile.toUtf8().constData(), 0);
        av_opt_set(p1->priv_data, "stats", statsFile.constData(), 0);
        bool p1ok = (avcodec_open2(p1, vcodec, nullptr) == 0);
        SwsContext *sws1 = p1ok ? sws_getContext(cropW, cropH, AV_PIX_FMT_RGBA, W, H, fmt.pix,
                                                  cropping ? SWS_BICUBIC : SWS_BILINEAR,
                                                  nullptr, nullptr, nullptr) : nullptr;
        AVFrame *vf1 = av_frame_alloc(); vf1->format = fmt.pix; vf1->width = W; vf1->height = H;
        av_frame_get_buffer(vf1, 0);
        AVPacket *pk1 = av_packet_alloc();
        QImage blk(W, H, QImage::Format_RGBA8888); blk.fill(Qt::black);
        for (int i = 0; p1ok && i < frameCount; ++i) {
            bool hc = false;
            QImage img = comp.renderFrame(frameAt(i), hc);
            if (img.isNull() || img.size() != QSize(W, H)) img = blk;
            if (img.format() != QImage::Format_RGBA8888) img = img.convertToFormat(QImage::Format_RGBA8888);
            if (av_frame_make_writable(vf1) < 0) { p1ok = false; break; }
            const int stride = int(img.bytesPerLine());
            const uint8_t *sd[4] = { img.constBits() + qint64(cropY) * stride + qint64(cropX) * 4,
                                     nullptr, nullptr, nullptr };
            int ss[4] = { stride, 0, 0, 0 };
            sws_scale(sws1, sd, ss, 0, cropH, vf1->data, vf1->linesize);
            vf1->pts = i;
            if (avcodec_send_frame(p1, vf1) < 0) { p1ok = false; break; }
            while (avcodec_receive_packet(p1, pk1) == 0) av_packet_unref(pk1);   // descarta paquetes
            if ((i & 31) == 0) emit progress(0.5 * double(i + 1) / frameCount);   // 1.ª mitad de la barra
        }
        if (p1ok) { avcodec_send_frame(p1, nullptr);
                    while (avcodec_receive_packet(p1, pk1) == 0) av_packet_unref(pk1); }
        av_frame_free(&vf1); av_packet_free(&pk1);
        if (sws1) sws_freeContext(sws1);
        avcodec_free_context(&p1);
        if (!p1ok) {
            avformat_free_context(oc);
            QFile::remove(QFile::decodeName(statsFile));
            fail(QStringLiteral("Fallo en la 1.ª pasada.")); return;
        }
        qInfo("[export] 2-pass: pasada 1 completa; pasada 2 (codificación)…");
    }

    // Formato de píxel que alimenta al encoder (y al swscale). AMF trabaja en NV12;
    // el resto en su pix nativo. En un fallback AMF→software se conserva NV12, que
    // libx264/libx265 también aceptan, para no desalinear el swscale.
    const AVPixelFormat encPix = fmt.pix;

    // Configura y abre un encoder de vídeo con las opciones propias de su familia
    // (software x264/x265/vpx o hardware AMD/AMF). Devuelve el contexto abierto, o
    // nullptr si no se pudo abrir (p. ej. AMF sin GPU/driver disponible).
    auto openVideoEncoder = [&](const AVCodec *codec, const FmtDesc &f) -> AVCodecContext * {
        AVCodecContext *c = avcodec_alloc_context3(codec);
        if (!c) return nullptr;
        c->width = W; c->height = H;
        c->pix_fmt = encPix;
        c->time_base = av_d2q(1.0 / job.fps, 1000000);
        c->framerate = av_d2q(job.fps, 1000000);
        c->gop_size = 12;
        // ProRes/DNxHR fijan la calidad por perfil. H.264/H.265/VP9: bitrate o CRF.
        if (f.useBitrate) {
            if (job.useCrf) {
                if (f.hwAmf) {   // AMF no tiene "crf": calidad constante = rc cqp + qp
                    av_opt_set(c->priv_data, "rc", "cqp", 0);
                    av_opt_set_int(c->priv_data, "qp_i", job.crf, 0);
                    av_opt_set_int(c->priv_data, "qp_p", job.crf, 0);
                    av_opt_set_int(c->priv_data, "qp_b", job.crf, 0);
                } else {
                    av_opt_set_int(c->priv_data, "crf", job.crf, 0);   // bit_rate=0
                }
            } else {
                c->bit_rate = job.videoBitrate;
                if (f.hwAmf)
                    av_opt_set(c->priv_data, "rc", "vbr_peak", 0);   // VBR con pico limitado
            }
        }
        if (oc->oformat->flags & AVFMT_GLOBALHEADER)
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        // Ajustes de velocidad/calidad por familia de encoder.
        if (f.vprofile)
            av_opt_set(c->priv_data, "profile", f.vprofile, 0);   // prores_ks / dnxhd
        if (f.hwAmf) {   // AMD/AMF: sin preset x264; usecase + preset de calidad AMF
            // "quality" (prefiere calidad) por defecto: para transcodificación de
            // archivo interesa el mejor uso del bitrate, no la velocidad máxima.
            av_opt_set(c->priv_data, "usage", "transcoding", 0);
            av_opt_set(c->priv_data, "quality", "quality", 0);
        } else if (qstrcmp(f.venc, "libx264") == 0 || qstrcmp(f.venc, "libx265") == 0) {
            av_opt_set(c->priv_data, "preset", "medium", 0);
            if (!job.videoProfile.isEmpty())
                av_opt_set(c->priv_data, "profile", job.videoProfile.toUtf8().constData(), 0);
        }
        if (qstrcmp(f.venc, "libvpx-vp9") == 0) {
            av_opt_set(c->priv_data, "deadline", "good", 0);
            av_opt_set_int(c->priv_data, "cpu-used", 4, 0);
        }
        if (twoPass) {   // 2.ª pasada: usa las stats de la 1.ª (solo x264/x265)
            c->flags |= AV_CODEC_FLAG_PASS2;
            av_opt_set(c->priv_data, "stats", statsFile.constData(), 0);
        }
        if (avcodec_open2(c, codec, nullptr) < 0) {
            avcodec_free_context(&c);
            return nullptr;
        }
        return c;
    };

    AVStream *vst = avformat_new_stream(oc, nullptr);
    AVCodecContext *vc = openVideoEncoder(vcodec, fmt);
    if (!vc && fmt.hwAmf) {
        // El encoder por hardware AMD no abrió (GPU ocupada, driver, o sin AMD):
        // se cae al equivalente por software para no abortar la exportación.
        const char *swId = (qstrcmp(fmt.venc, "hevc_amf") == 0) ? "h265" : "h264";
        const FmtDesc &swf = fmtById(QLatin1String(swId));
        qWarning("[export] encoder AMD '%s' no disponible; se usa software '%s'.",
                 fmt.venc, swf.venc);
        if (const AVCodec *swc = avcodec_find_encoder_by_name(swf.venc))
            vc = openVideoEncoder(swc, swf);
    }
    if (!vc) {
        avformat_free_context(oc);
        fail(QStringLiteral("No se pudo abrir el encoder de vídeo (%1).").arg(QLatin1String(fmt.venc))); return;
    }
    avcodec_parameters_from_context(vst->codecpar, vc);
    vst->time_base = vc->time_base;

    // --- Encoder de audio (AAC), opcional si hay mezcla ---
    const bool hasAudio = !job.audioS16.isEmpty();
    AVStream *ast = nullptr;
    AVCodecContext *ac = nullptr;
    if (hasAudio) {
        const AVCodec *acodec = avcodec_find_encoder_by_name(fmt.aenc);
        if (acodec) {
            ast = avformat_new_stream(oc, nullptr);
            ac = avcodec_alloc_context3(acodec);
            ac->sample_fmt = fmt.asfmt;   // FLTP (AAC) o S16 (PCM/Opus)
            ac->sample_rate = 48000;
            // Canales: 1 mono · 2 estéreo · 6 (5.1) · 8 (7.1), por upmix. Opus multicanal
            // no se soporta aquí → estéreo.
            int chOut = job.channels;
            if (chOut != 1 && chOut != 2 && chOut != 6 && chOut != 8) chOut = 2;
            if (chOut > 2 && qstrcmp(fmt.aenc, "libopus") == 0) chOut = 2;
            av_channel_layout_default(&ac->ch_layout, chOut);   // 6 → 5.1, 8 → 7.1
            if (qstrcmp(fmt.aenc, "pcm_s16le") != 0)
                ac->bit_rate = job.audioBitrate;   // PCM no usa bitrate
            ac->time_base = AVRational{ 1, 48000 };
            if (oc->oformat->flags & AVFMT_GLOBALHEADER)
                ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(ac, acodec, nullptr) < 0) {
                avcodec_free_context(&ac); ast = nullptr;
            } else {
                avcodec_parameters_from_context(ast->codecpar, ac);
                ast->time_base = AVRational{ 1, 48000 };
            }
        }
    }

    // --- Abrir archivo y cabecera ---
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, pathUtf8.constData(), AVIO_FLAG_WRITE) < 0) {
            if (ac) avcodec_free_context(&ac);
            avcodec_free_context(&vc); avformat_free_context(oc);
            fail(QStringLiteral("No se pudo abrir el archivo de salida.")); return;
        }
    }
    if (avformat_write_header(oc, nullptr) < 0) {
        if (ac) avcodec_free_context(&ac);
        avcodec_free_context(&vc);
        if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
        avformat_free_context(oc);
        fail(QStringLiteral("Error al escribir la cabecera del MP4.")); return;
    }

    // --- Conversión de color RGBA → pix_fmt del encoder (con recorte de salida) ---
    SwsContext *sws = sws_getContext(cropW, cropH, AV_PIX_FMT_RGBA, W, H, encPix,
                                     cropping ? SWS_BICUBIC : SWS_BILINEAR, nullptr, nullptr, nullptr);
    AVFrame *vframe = av_frame_alloc();
    vframe->format = encPix; vframe->width = W; vframe->height = H;
    av_frame_get_buffer(vframe, 0);
    AVPacket *pkt = av_packet_alloc();

    // --- Audio maestro deinterleaveado a float planar (L, R) ---
    QVector<float> aL, aR;
    int aTotal = 0, aFrameSize = 0;
    AVFrame *aframe = nullptr;
    int aStart = 0;   // muestra siguiente a codificar
    if (ac) {
        aTotal = int(job.audioS16.size() / 4); // 2 canales * S16
        aL.resize(aTotal); aR.resize(aTotal);
        const int16_t *s = reinterpret_cast<const int16_t *>(job.audioS16.constData());
        for (int k = 0; k < aTotal; ++k) {
            aL[k] = s[2 * k] / 32768.0f;
            aR[k] = s[2 * k + 1] / 32768.0f;
        }
        aFrameSize = ac->frame_size > 0 ? ac->frame_size : 1024;
        aframe = av_frame_alloc();
        aframe->format = ac->sample_fmt;   // FLTP (AAC) o S16 empaquetado (PCM/Opus)
        av_channel_layout_copy(&aframe->ch_layout, &ac->ch_layout);
        aframe->sample_rate = 48000;
        aframe->nb_samples = aFrameSize;
        av_frame_get_buffer(aframe, 0);
    }

    // Codifica los fotogramas de audio cuyo tiempo cae antes de untilSec.
    auto encodeAudioUntil = [&](double untilSec) -> bool {
        if (!ac) return true;
        while (aStart < aTotal && (double(aStart) / 48000.0) < untilSec) {
            const int n = std::min(aFrameSize, aTotal - aStart);
            if (av_frame_make_writable(aframe) < 0) return false;
            aframe->nb_samples = n;
            const int ch = aframe->ch_layout.nb_channels;   // 1 mono · 2 estéreo · 6 (5.1)
            const bool s16 = (ac->sample_fmt == AV_SAMPLE_FMT_S16);
            for (int k = 0; k < n; ++k) {
                const float l = aL[aStart + k], r = aR[aStart + k];
                // Valores por canal. 5.1/7.1 = upmix pasivo. Orden FFmpeg:
                //  5.1: FL FR FC LFE BL BR   ·   7.1: FL FR FC LFE BL BR SL SR
                float v[8];
                if (ch == 1) { v[0] = (l + r) * 0.5f; }
                else if (ch == 6) {
                    v[0] = l; v[1] = r; v[2] = (l + r) * 0.5f; v[3] = 0.0f; v[4] = l * 0.6f; v[5] = r * 0.6f;
                } else if (ch == 8) {
                    v[0] = l; v[1] = r; v[2] = (l + r) * 0.5f; v[3] = 0.0f;
                    v[4] = l * 0.5f; v[5] = r * 0.5f; v[6] = l * 0.6f; v[7] = r * 0.6f;
                } else { v[0] = l; v[1] = r; }
                if (s16) {   // empaquetado intercalado
                    auto *d = reinterpret_cast<int16_t *>(aframe->data[0]);
                    for (int c = 0; c < ch; ++c)
                        d[k * ch + c] = int16_t(std::clamp(int(std::lround(v[c] * 32767.0f)), -32768, 32767));
                } else {     // planar float (un plano por canal)
                    for (int c = 0; c < ch; ++c)
                        reinterpret_cast<float *>(aframe->data[c])[k] = v[c];
                }
            }
            aframe->pts = aStart;
            if (avcodec_send_frame(ac, aframe) < 0) return false;
            if (!drainEncoder(oc, ac, ast, pkt)) return false;
            aStart += n;
        }
        return true;
    };

    // --- Bucle principal: componer + codificar cada fotograma ---
    // En 2 pasadas, esta es la 2.ª: la barra va del 50 % al 100 %.
    const double progBase = twoPass ? 0.5 : 0.0, progSpan = twoPass ? 0.5 : 1.0;
    bool ok = true;
    QImage black(W, H, QImage::Format_RGBA8888);
    black.fill(Qt::black);
    for (int i = 0; i < frameCount && ok; ++i) {
        bool hasContent = false;
        QImage img = comp.renderFrame(frameAt(i), hasContent);   // streaming o materializado
        if (img.isNull() || img.size() != QSize(W, H))
            img = black;
        if (img.format() != QImage::Format_RGBA8888)
            img = img.convertToFormat(QImage::Format_RGBA8888);

        if (av_frame_make_writable(vframe) < 0) { ok = false; break; }
        const int stride = int(img.bytesPerLine());
        const uint8_t *srcData[4] = { img.constBits() + qint64(cropY) * stride + qint64(cropX) * 4,
                                      nullptr, nullptr, nullptr };
        int srcStride[4] = { stride, 0, 0, 0 };
        sws_scale(sws, srcData, srcStride, 0, cropH, vframe->data, vframe->linesize);
        vframe->pts = i;
        if (avcodec_send_frame(vc, vframe) < 0) { ok = false; break; }
        if (!drainEncoder(oc, vc, vst, pkt)) { ok = false; break; }

        // Audio hasta el final de este fotograma (mantiene A/V intercalados).
        if (!encodeAudioUntil(double(i + 1) / job.fps)) { ok = false; break; }

        if ((i & 7) == 0 || i == frameCount - 1)
            emit progress(progBase + progSpan * double(i + 1) / frameCount);
    }

    // --- Vaciado (flush) de ambos encoders ---
    if (ok && ac) ok = encodeAudioUntil(1e18); // resto del audio
    if (ok) { avcodec_send_frame(vc, nullptr); ok = drainEncoder(oc, vc, vst, pkt); }
    if (ok && ac) { avcodec_send_frame(ac, nullptr); ok = drainEncoder(oc, ac, ast, pkt); }
    if (ok) av_write_trailer(oc);

    // --- Limpieza ---
    if (aframe) av_frame_free(&aframe);
    av_frame_free(&vframe);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    if (ac) avcodec_free_context(&ac);
    avcodec_free_context(&vc);
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    avformat_free_context(oc);

    if (twoPass) {   // limpia los ficheros de estadísticas de la 1.ª pasada
        const QString sf = QFile::decodeName(statsFile);
        QFile::remove(sf);
        QFile::remove(sf + QStringLiteral(".mbtree"));
        QFile::remove(sf + QStringLiteral(".cutree"));
    }

    if (ok) {
        qInfo("[export] completado: %s (%lld bytes)",
              qUtf8Printable(job.path), qint64(QFileInfo(job.path).size()));
        emit finished(true, job.path);
    } else {
        qCritical("[export] error durante la codificación de %s", qUtf8Printable(job.path));
        emit finished(false, QStringLiteral("Fallo durante la codificación."));
    }
}

// ============================ Exporter (fachada) ============================

Exporter::Exporter(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<ExportJob>("ExportJob");
    m_thread = new QThread(this);
    m_worker = new ExportWorker;
    m_worker->moveToThread(m_thread);
    connect(this, &Exporter::requestRun, m_worker, &ExportWorker::run);
    connect(m_worker, &ExportWorker::progress, this, [this](double f) {
        m_progress = f; emit progressChanged();
    });
    connect(m_worker, &ExportWorker::finished, this, [this](bool ok, const QString &msg) {
        m_running = false; m_progress = ok ? 1.0 : m_progress;
        emit progressChanged();
        emit runningChanged();
        // Si venimos de la cola de render: marca el trabajo y pasa al siguiente.
        if (m_queueRunning && m_curQueueIdx >= 0 && m_curQueueIdx < m_queue.size()) {
            m_queue[m_curQueueIdx].status = ok ? 2 : 3;
            m_curQueueIdx = -1;
            emit queueChanged();
            renderNextInQueue();
            return;
        }
        setStatus(ok ? QStringLiteral("Exportado: %1").arg(QFileInfo(msg).fileName())
                     : QStringLiteral("Error: %1").arg(msg));
    });
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

Exporter::~Exporter()
{
    m_thread->quit();
    m_thread->wait();
}

void Exporter::setSources(TimelineModel *timeline) { m_timeline = timeline; }

void Exporter::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s; emit statusChanged();
}

// ---- Ajustes de salida (chips de la StatusBar) ----
void Exporter::bumpCustom()
{
    m_preset = QStringLiteral("Personalizado");
    emit settingsChanged();
}

void Exporter::setResolution(int w, int h)
{
    if (w <= 0 || h <= 0 || (w == m_outW && h == m_outH)) return;
    m_outW = w; m_outH = h;
    bumpCustom();
}

void Exporter::setOutFps(double fps)
{
    if (fps <= 0 || qFuzzyCompare(fps, m_outFps)) return;
    m_outFps = fps;
    bumpCustom();
}

void Exporter::setVideoMbps(int mbps)
{
    if (mbps <= 0 || mbps == m_mbps) return;
    m_mbps = mbps;
    bumpCustom();
}

void Exporter::setAudioKbps(int kbps)
{
    if (kbps < 0) kbps = 0;
    if (kbps == m_audioKbps) return;
    m_audioKbps = kbps;
    emit settingsChanged();
}

void Exporter::setAudioChannels(int ch)
{
    if (ch != 1 && ch != 2 && ch != 6 && ch != 8) ch = 2;   // mono · estéreo · 5.1 · 7.1
    if (ch == m_audioChannels) return;
    m_audioChannels = ch;
    emit settingsChanged();
}

// Recorte de salida: cada margen en [0..45] % (deja al menos ~10 % de imagen por eje).
void Exporter::setCropTop(int pct)
{
    pct = qBound(0, pct, 45);
    if (pct == m_cropTop) return;
    m_cropTop = pct; emit settingsChanged();
}
void Exporter::setCropBottom(int pct)
{
    pct = qBound(0, pct, 45);
    if (pct == m_cropBottom) return;
    m_cropBottom = pct; emit settingsChanged();
}
void Exporter::setCropLeft(int pct)
{
    pct = qBound(0, pct, 45);
    if (pct == m_cropLeft) return;
    m_cropLeft = pct; emit settingsChanged();
}
void Exporter::setCropRight(int pct)
{
    pct = qBound(0, pct, 45);
    if (pct == m_cropRight) return;
    m_cropRight = pct; emit settingsChanged();
}

void Exporter::setCrfEnabled(bool on)
{
    if (on == m_crfEnabled) return;
    m_crfEnabled = on;
    emit settingsChanged();
}

void Exporter::setCrf(int v)
{
    v = qBound(0, v, 51);
    if (v == m_crf) return;
    m_crf = v;
    emit settingsChanged();
}

void Exporter::setTwoPass(bool on)
{
    if (on == m_twoPass) return;
    m_twoPass = on;
    emit settingsChanged();
}

void Exporter::setVideoProfile(const QString &p)
{
    if (p == m_videoProfile) return;
    m_videoProfile = p;
    emit settingsChanged();
}

void Exporter::applyPreset(const QString &name, int w, int h, double fps, int mbps)
{
    if (w <= 0 || h <= 0 || fps <= 0 || mbps <= 0) return;
    m_outW = w; m_outH = h; m_outFps = fps; m_mbps = mbps;
    m_preset = name;
    emit settingsChanged();
}

bool Exporter::buildJob(const QString &path, int w, int h, double fps, int mbps, int audioKbps,
                        qint64 startUs, qint64 durUs, ExportJob &job)
{
    if (!m_timeline) return false;
    if (startUs < 0) startUs = 0;
    if (durUs <= 0) durUs = m_timeline->contentEndUs() - startUs;   // hasta el fin
    if (durUs <= 0) return false;

    job = ExportJob{};
    job.path = path;
    job.width = w; job.height = h; job.fps = fps;
    job.videoBitrate = mbps * 1'000'000;
    if (audioKbps > 0) job.audioBitrate = audioKbps * 1000;
    job.durationUs = durUs;

    // Vídeo en STREAMING: se guarda una instantánea del timeline y el worker resuelve
    // cada fotograma bajo demanda (no se materializan los cientos de miles de listas).
    const int frameCount = int(std::ceil((durUs / 1e6) * fps));
    const double estAudioMB = (durUs / 1e6) * 48000.0 * 4.0 / (1024 * 1024);   // master S16
    qInfo("[export] buildJob: rango %.1f-%.1f s, %.3f fps => %d fotogramas (%dx%d); audio ~%.0f MB",
          startUs / 1e6, (startUs + durUs) / 1e6, fps, frameCount, w, h, estAudioMB);
    if (estAudioMB > 512.0)
        qWarning("[export] AVISO: exportación larga (%d fotogramas, master ~%.0f MB).", frameCount, estAudioMB);
    job.streamFrames = true;
    job.frameCount = frameCount;
    job.startUs = startUs;
    job.snapshot = m_timeline->renderSnapshot();   // copia ligera (O(clips), no O(fotogramas))
    qInfo("[export] instantánea de timeline lista (%lld clips, %lld pistas)",
          qint64(job.snapshot.clips.size()), qint64(job.snapshot.tracks.size()));

    // audioKbps<=0: exportar sin pista de audio.
    if (audioKbps <= 0)
        return true;

    // Instantánea de audio: hornea la mezcla maestra hasta el fin del rango y recorta
    // el tramo [startUs, startUs+durUs) (48 kHz S16 estéreo → 4 bytes por muestra).
    QVector<AudioMixClip> mix;
    for (const TimelineModel::AudioClip &a : m_timeline->audioClips()) {
        AudioMixClip m;
        m.mediaPath = a.mediaPath; m.trackIndex = a.trackIndex;
        m.startUs = a.startUs; m.durationUs = a.durationUs; m.inUs = a.inUs;
        m.speed = a.speed; m.gain = a.gain; m.pan = a.pan; m.mute = a.mute;
        for (const TimelineModel::Keyframe &k : a.gainKf) m.gainKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.panKf) m.panKf.push_back({ k.sourceUs, k.value });
        m.eqOn = a.eqOn; m.eqLowDb = a.eqLowDb; m.eqMidDb = a.eqMidDb; m.eqHighDb = a.eqHighDb;
        m.compOn = a.compOn; m.compThreshDb = a.compThreshDb;
        m.compRatio = a.compRatio; m.compMakeupDb = a.compMakeupDb;
        m.gateOn = a.gateOn; m.gateThreshDb = a.gateThreshDb;
        m.deEssOn = a.deEssOn; m.deEssThreshDb = a.deEssThreshDb;
        m.reverbOn = a.reverbOn; m.reverbMix = a.reverbMix; m.reverbSize = a.reverbSize;
        m.clipEqOn = a.clipEqOn; m.clipEqLowDb = a.clipEqLowDb; m.clipEqMidDb = a.clipEqMidDb; m.clipEqHighDb = a.clipEqHighDb;
        m.clipCompOn = a.clipCompOn; m.clipCompThreshDb = a.clipCompThreshDb;
        m.clipCompRatio = a.clipCompRatio; m.clipCompMakeupDb = a.clipCompMakeupDb;
        m.clipGateOn = a.clipGateOn; m.clipGateThreshDb = a.clipGateThreshDb;
        m.clipDeEssOn = a.clipDeEssOn; m.clipDeEssThreshDb = a.clipDeEssThreshDb;
        m.clipReverbOn = a.clipReverbOn; m.clipReverbMix = a.clipReverbMix; m.clipReverbSize = a.clipReverbSize;
        for (const TimelineModel::Keyframe &k : a.eqLowKf)  m.clipEqLowKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.eqMidKf)  m.clipEqMidKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.eqHighKf) m.clipEqHighKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.reverbMixKf) m.clipReverbMixKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.compThreshKf) m.clipCompThreshKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.compRatioKf)  m.clipCompRatioKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.compMakeupKf) m.clipCompMakeupKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.gateThreshKf) m.clipGateThreshKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.deEssThreshKf) m.clipDeEssThreshKf.push_back({ k.sourceUs, k.value });
        mix.push_back(m);
    }
    {
        AudioPlayer baker;
        baker.setMix(mix, startUs + durUs, m_timeline->masterGain(), m_timeline->masterPan(),
                     m_timeline->masterLimiterOn(), m_timeline->masterCeilingDb());
        const QByteArray &master = baker.master();
        const qint64 startByte = qint64(std::llround(startUs / 1e6 * 48000.0)) * 4;
        const qint64 lenByte = qint64(std::llround(durUs / 1e6 * 48000.0)) * 4;
        if (startByte < master.size())
            job.audioS16 = master.mid(int(startByte), int(qMin(lenByte, master.size() - startByte)));
    }
    return true;
}

bool Exporter::buildCopyJob(const QString &path, qint64 startUs, qint64 durUs, ExportJob &job)
{
    if (!m_timeline) return false;
    if (startUs < 0) startUs = 0;
    if (durUs <= 0) durUs = m_timeline->contentEndUs() - startUs;   // hasta el fin
    if (durUs <= 0) return false;
    const qint64 endUs = startUs + durUs;

    // Localiza el clip de vídeo (con media) que cubre TODO el rango a velocidad normal.
    // Si hay varios solapados (multi-pista), toma el de la pista más alta (el visible).
    const TimelineModel::RenderSnapshot snap = m_timeline->renderSnapshot();
    const TimelineModel::Clip *best = nullptr;
    for (const TimelineModel::Clip &c : snap.clips) {
        if (c.kind != QLatin1String("video") || c.mediaPath.isEmpty()) continue;
        if (qAbs(c.speed - 1.0) > 1e-6) continue;                              // remapeo no copiable
        if (c.startUs > startUs || c.startUs + c.durationUs < endUs) continue; // no cubre el rango
        if (!best || c.trackIndex > best->trackIndex) best = &c;
    }
    if (!best) {
        setStatus(QStringLiteral("Copia directa: el rango debe estar cubierto por un único clip de vídeo "
                                 "a velocidad normal. Para composiciones o efectos usa H.264/H.265."));
        return false;
    }

    job = ExportJob{};
    job.path = path;
    job.format = QStringLiteral("copy");
    job.streamCopy = true;
    job.copySrcPath = best->mediaPath;
    job.copySrcStartUs = best->inUs + (startUs - best->startUs);   // timeline → tiempo de origen
    job.copySrcEndUs   = best->inUs + (endUs   - best->startUs);
    job.durationUs = durUs;
    qInfo("[export] buildCopyJob: %s, origen %.1f-%.1f s (rango timeline %.1f-%.1f s)",
          qUtf8Printable(best->mediaPath), job.copySrcStartUs / 1e6, job.copySrcEndUs / 1e6,
          startUs / 1e6, endUs / 1e6);
    return true;
}

void Exporter::exportTimeline(const QString &path, int width, int height,
                              double fps, double seconds)
{
    if (m_running || m_queueRunning || !m_timeline || path.isEmpty()) return;
    if (width <= 0 || height <= 0 || fps <= 0) return;

    // Rango: si se pide `seconds` explícito exporta [0, seconds]; si no, el rango I/O.
    qint64 startUs = 0, durUs = 0;
    if (seconds > 0) { durUs = qint64(seconds * 1e6); }
    else { startUs = m_timeline->exportStartUs(); durUs = m_timeline->exportEndUs() - startUs; }
    ExportJob job;
    if (m_format == QLatin1String("copy")) {
        if (!buildCopyJob(path, startUs, durUs, job))
            return;   // buildCopyJob ya fijó el status con el motivo
    } else {
        if (!buildJob(path, width, height, fps, m_mbps, m_audioKbps, startUs, durUs, job)) {
            setStatus(QStringLiteral("No se pudo preparar la exportación (rango vacío o memoria insuficiente; ver el log)."));
            return;
        }
        job.format = m_format;
        job.channels = m_audioChannels; job.useCrf = m_crfEnabled; job.crf = m_crf; job.twoPass = m_twoPass;
        job.videoProfile = m_videoProfile;
        job.cropTop = m_cropTop / 100.0; job.cropBottom = m_cropBottom / 100.0;
        job.cropLeft = m_cropLeft / 100.0; job.cropRight = m_cropRight / 100.0;
    }

    m_running = true; m_progress = 0.0;
    setStatus(QStringLiteral("Exportando…"));
    emit runningChanged();
    emit progressChanged();
    emit requestRun(job);
}

// ==================== Ruta de salida y cola de render (página Entregar) ====================

QString Exporter::formatLabel() const { return QString::fromLatin1(fmtById(m_format).label); }
QString Exporter::outExt() const { return QString::fromLatin1(fmtById(m_format).ext); }
bool Exporter::formatUsesBitrate() const { return fmtById(m_format).useBitrate; }

QVariantList Exporter::availableFormats() const
{
    QVariantList out;
    for (const auto &f : kFormats)
        if (f.streamCopy || (avcodec_find_encoder_by_name(f.venc) && (!f.hwAmf || amfUsable())))
            out.append(QVariantMap{ { "id", QString::fromLatin1(f.id) },
                                    { "label", QString::fromLatin1(f.label) },
                                    { "ext", QString::fromLatin1(f.ext) } });
    return out;
}

void Exporter::setFormat(const QString &id)
{
    if (id == m_format) return;
    bool ok = false;   // copia directa siempre; el resto exige encoder (y GPU AMD si es AMF)
    for (const auto &f : kFormats)
        if (id == QLatin1String(f.id)
            && (f.streamCopy || (avcodec_find_encoder_by_name(f.venc) && (!f.hwAmf || amfUsable())))) {
            ok = true; break;
        }
    if (!ok) return;
    m_format = id;
    emit settingsChanged();
}

QVariantList Exporter::queue() const
{
    QVariantList out;
    for (const QueueItem &q : m_queue)
        out.append(QVariantMap{
            { "name", q.name }, { "path", q.path }, { "preset", q.preset },
            { "format", q.format }, { "width", q.width }, { "height", q.height },
            { "fps", q.fps }, { "mbps", q.mbps }, { "status", q.status } });
    return out;
}

void Exporter::setOutName(const QString &name)
{
    const QString n = name.trimmed();
    if (n == m_outName) return;
    m_outName = n;
    emit settingsChanged();
}

QString Exporter::absoluteOutputPath() const
{
    const QString base = m_outName.isEmpty() ? QStringLiteral("pepe_export") : m_outName;
    const QString file = base + QLatin1Char('.') + outExt();
    return m_outDir.isEmpty() ? file : m_outDir + QLatin1Char('/') + file;
}

void Exporter::chooseOutputFile()
{
#ifdef _WIN32
    wchar_t file[MAX_PATH];
    lstrcpynW(file, reinterpret_cast<const wchar_t *>(absoluteOutputPath().utf16()), MAX_PATH);
    // Filtro y extensión por defecto según el formato elegido (doble-nulo requerido).
    const QString ext = outExt();
    std::wstring filt = (QStringLiteral("Vídeo (*.%1)").arg(ext)).toStdWString();
    filt.push_back(L'\0'); filt += (QStringLiteral("*.") + ext).toStdWString();
    filt.push_back(L'\0'); filt += L"Todos (*.*)";
    filt.push_back(L'\0'); filt += L"*.*"; filt.push_back(L'\0'); filt.push_back(L'\0');
    wchar_t extW[16]; lstrcpynW(extW, reinterpret_cast<const wchar_t *>(ext.utf16()), 16);
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filt.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = extW;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        const QFileInfo fi(QString::fromWCharArray(file));
        m_outDir = fi.absolutePath();
        m_outName = fi.completeBaseName();
        emit settingsChanged();
    }
#else
    setStatus(QStringLiteral("Diálogo de salida solo disponible en Windows."));
#endif
}

void Exporter::enqueueCurrent()
{
    if (!m_timeline || m_timeline->contentEndUs() <= 0) {
        setStatus(QStringLiteral("La línea de tiempo está vacía.")); return;
    }
    const QString base = m_outName.isEmpty() ? QStringLiteral("pepe_export") : m_outName;
    // Nombre único dentro de la cola (…, _2, _3) para no pisar salidas.
    QString name = base; int n = 2;
    auto taken = [this](const QString &nm) {
        for (const QueueItem &q : m_queue) if (q.name == nm) return true;
        return false;
    };
    while (taken(name)) name = base + QStringLiteral("_") + QString::number(n++);
    QueueItem it;
    it.name = name;
    const QString file = name + QLatin1Char('.') + outExt();
    it.path = m_outDir.isEmpty() ? file : m_outDir + QLatin1Char('/') + file;
    it.preset = m_preset;
    it.format = m_format;
    it.width = m_outW; it.height = m_outH; it.fps = m_outFps; it.mbps = m_mbps;
    it.audioKbps = m_audioKbps;
    it.channels = m_audioChannels; it.useCrf = m_crfEnabled; it.crf = m_crf; it.twoPass = m_twoPass;
    it.videoProfile = m_videoProfile;
    it.cropTop = m_cropTop; it.cropBottom = m_cropBottom;
    it.cropLeft = m_cropLeft; it.cropRight = m_cropRight;
    it.startUs = m_timeline->exportStartUs();
    it.durUs = m_timeline->exportEndUs() - it.startUs;
    it.status = 0;
    m_queue.push_back(it);
    emit queueChanged();
}

void Exporter::removeFromQueue(int index)
{
    if (index < 0 || index >= m_queue.size()) return;
    if (m_queue[index].status == 1) return;   // no quitar el que se está renderizando
    m_queue.remove(index);
    emit queueChanged();
}

void Exporter::clearQueue()
{
    // Conserva el trabajo en curso; elimina el resto.
    QVector<QueueItem> keep;
    for (const QueueItem &q : m_queue) if (q.status == 1) keep.push_back(q);
    m_queue = keep;
    emit queueChanged();
}

void Exporter::startQueue()
{
    if (m_running || m_queueRunning) return;
    bool anyPending = false;
    for (const QueueItem &q : m_queue) if (q.status == 0) { anyPending = true; break; }
    if (!anyPending) { setStatus(QStringLiteral("La cola no tiene trabajos pendientes.")); return; }
    m_queueRunning = true;
    emit queueChanged();
    renderNextInQueue();
}

void Exporter::renderNextInQueue()
{
    // Busca el primer trabajo pendiente.
    int idx = -1;
    for (int i = 0; i < m_queue.size(); ++i)
        if (m_queue[i].status == 0) { idx = i; break; }
    if (idx < 0) {   // no queda nada: cola completada
        m_queueRunning = false;
        m_curQueueIdx = -1;
        setStatus(QStringLiteral("Cola completada."));
        emit queueChanged();
        return;
    }

    QueueItem &it = m_queue[idx];
    ExportJob job;
    bool built = false;
    if (it.format == QLatin1String("copy")) {
        built = buildCopyJob(it.path, it.startUs, it.durUs, job);
    } else if (buildJob(it.path, it.width, it.height, it.fps, it.mbps, it.audioKbps, it.startUs, it.durUs, job)) {
        job.format = it.format;
        job.channels = it.channels; job.useCrf = it.useCrf; job.crf = it.crf; job.twoPass = it.twoPass;
        job.videoProfile = it.videoProfile;
        job.cropTop = it.cropTop / 100.0; job.cropBottom = it.cropBottom / 100.0;
        job.cropLeft = it.cropLeft / 100.0; job.cropRight = it.cropRight / 100.0;
        built = true;
    }
    if (!built) {
        it.status = 3;   // error (rango vacío o no copiable)
        emit queueChanged();
        renderNextInQueue();
        return;
    }
    it.status = 1;   // en curso
    m_curQueueIdx = idx;
    m_running = true; m_progress = 0.0;
    setStatus(QStringLiteral("Renderizando cola: %1").arg(it.name));
    emit queueChanged();
    emit runningChanged();
    emit progressChanged();
    emit requestRun(job);
}

void Exporter::exportNow()
{
    if (m_running || m_queueRunning) return;
    if (m_outDir.isEmpty()) { openExportDialog(); return; }   // sin carpeta: pide una
    exportTimeline(absoluteOutputPath(), m_outW, m_outH, m_outFps, 0.0);
}

void Exporter::openExportDialog()
{
    if (m_running || m_queueRunning) return;
#ifdef _WIN32
    wchar_t file[MAX_PATH] = L"pepe_export.mp4";
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Vídeo MP4 (*.mp4)\0*.mp4\0Todos (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"mp4";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        const QString path = QString::fromWCharArray(file);
        exportTimeline(path, m_outW, m_outH, m_outFps, 0.0);   // ajustes de los chips
    }
#else
    setStatus(QStringLiteral("Diálogo de exportación solo disponible en Windows."));
#endif
}

// ============================ Auto-test ============================

bool pvsWriteColorTestMp4(const QString &path)
{
    ExportJob job;
    job.path = path;
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
    bool ok = false;
    QObject::connect(&worker, &ExportWorker::finished,
                     [&ok](bool o, const QString &) { ok = o; });
    worker.run(job);
    return ok && QFileInfo(job.path).size() > 1024;
}

int runExportSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_EXPORT_SELFTEST"))
        return -1;

    // Un trabajo sintético: 2 s @ 24 fps, 320x180, un clip de color a pantalla completa.
    ExportJob job;
    job.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_export_selftest.mp4"));
    job.width = 320; job.height = 180; job.fps = 24.0; job.durationUs = 2'000'000;
    const int frames = 48;
    for (int i = 0; i < frames; ++i) {
        TimelineModel::RenderClip rc;
        rc.trackIndex = 0; rc.kind = QStringLiteral("video");
        rc.fill = (i % 2) ? QStringLiteral("#3060c0") : QStringLiteral("#c06030");
        rc.sourceUs = 0;
        job.frames.push_back(QVector<TimelineModel::RenderClip>{ rc });
    }
    // Audio: 2 s de silencio estéreo S16 a 48 kHz (verifica también la ruta AAC).
    job.audioS16 = QByteArray(48000 * 2 * 2 * 2, '\0');

    QFile::remove(job.path);
    ExportWorker worker;
    bool done = false, ok = false; QString msg;
    QObject::connect(&worker, &ExportWorker::finished, [&](bool o, const QString &m) {
        ok = o; msg = m; done = true;
    });
    worker.run(job);

    const QFileInfo fi(job.path);
    const bool exists = fi.exists() && fi.size() > 1024;

    // Ajustes de salida (chips): preset → valores; cambio manual → "Personalizado".
    Exporter ex;
    ex.applyPreset(QStringLiteral("YouTube 4K"), 3840, 2160, 30.0, 40);
    const bool presetOk = ex.outWidth() == 3840 && ex.outHeight() == 2160
                          && ex.videoMbps() == 40
                          && ex.presetName() == QStringLiteral("YouTube 4K");
    ex.setVideoMbps(24);
    const bool customOk = ex.videoMbps() == 24
                          && ex.presetName() == QStringLiteral("Personalizado");
    qInfo("[EXPORT selftest] ajustes: preset=%s manual=%s",
          presetOk ? "OK" : "FALLO", customOk ? "OK" : "FALLO");

    const bool pass = done && ok && exists && presetOk && customOk;
    qInfo("[EXPORT selftest] ok=%d archivo=%s tam=%lld  => %s",
          int(ok), qUtf8Printable(job.path), exists ? fi.size() : -1,
          pass ? "PASS" : "FALLO");
    return pass ? 0 : 1;
}

int runDeliverSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_DELIVER_SELFTEST"))
        return -1;

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char *what) {
        if (ok) ++pass; else { ++fail; qWarning("[DELIVER selftest] FALLO: %s", what); }
    };

    // Timeline con contenido mínimo: un título en el playhead 0 (aporta contentEndUs).
    TimelineModel tl;
    tl.setPlayheadUs(0);
    tl.addTitleAtPlayhead();
    check(tl.contentEndUs() > 0, "timeline con contenido");

    // Salida en temp: CWD temporal para resolver las rutas relativas de la cola.
    const QString prevCwd = QDir::currentPath();
    QDir::setCurrent(QDir::tempPath());
    const QString p1 = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_deliver.mp4"));
    const QString p2 = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_deliver_2.mp4"));
    QFile::remove(p1); QFile::remove(p2);

    Exporter ex;
    ex.setSources(&tl);
    ex.setResolution(128, 72);
    ex.setOutFps(6.0);
    ex.setOutName(QStringLiteral("pvs_deliver"));
    ex.enqueueCurrent();   // pvs_deliver: contenido completo (~15 s)
    // El segundo trabajo usa un rango de entrada/salida (2–4 s): debe salir más corto.
    tl.setPlayheadUs(2'000'000); tl.setMarkInAtPlayhead();
    tl.setPlayheadUs(4'000'000); tl.setMarkOutAtPlayhead();
    ex.enqueueCurrent();   // pvs_deliver_2 (rango I/O, nombre único autogenerado)
    check(ex.queue().size() == 2, "dos trabajos encolados con nombres únicos");

    // Renderiza la cola y espera (bucle de eventos) a que termine.
    QEventLoop loop;
    QObject::connect(&ex, &Exporter::queueChanged, &loop, [&]() {
        if (!ex.queueRunning()) loop.quit();
    });
    QTimer::singleShot(90000, &loop, &QEventLoop::quit);   // tope de seguridad
    ex.startQueue();
    if (ex.queueRunning()) loop.exec();

    const bool ok1 = QFileInfo(p1).exists() && QFileInfo(p1).size() > 1024;
    const bool ok2 = QFileInfo(p2).exists() && QFileInfo(p2).size() > 1024;
    check(ok1, "el primer trabajo de la cola produjo un MP4");
    check(ok2, "el segundo trabajo de la cola produjo un MP4");
    // El trabajo con rango I/O (2–4 s) es más corto que el completo (~15 s).
    check(ok1 && ok2 && QFileInfo(p2).size() < QFileInfo(p1).size(),
          "el trabajo con rango I/O produce un archivo más corto");
    const QVariantList q = ex.queue();
    check(q.size() == 2 && q[0].toMap()[QStringLiteral("status")].toInt() == 2
              && q[1].toMap()[QStringLiteral("status")].toInt() == 2,
          "ambos trabajos quedan marcados como hechos");
    check(!ex.queueRunning(), "la cola queda parada al terminar");

    QDir::setCurrent(prevCwd);
    QFile::remove(p1); QFile::remove(p2);

    qWarning("[DELIVER selftest] %d OK, %d FALLO  => %s", pass, fail, fail == 0 ? "PASS" : "FALLO");
    return fail == 0 ? 0 : 1;
}

int runCodecSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_CODEC_SELFTEST"))
        return -1;

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char *what) {
        qInfo("[CODEC selftest] %-8s %s", what, ok ? "OK" : "FAIL");
        if (ok) ++pass; else ++fail;
    };

    const int W = 320, H = 180;
    const double fps = 24.0;
    const QByteArray audio(48000 * 2 * 2, '\0');   // 1 s de silencio S16 estéreo

    auto makeFrames = []() {
        QVector<QVector<TimelineModel::RenderClip>> fr;
        for (int i = 0; i < 24; ++i) {
            TimelineModel::RenderClip rc;
            rc.trackIndex = 0; rc.kind = QStringLiteral("video");
            rc.fill = (i % 2) ? QStringLiteral("#3060c0") : QStringLiteral("#c06030");
            rc.sourceUs = 0;
            fr.push_back(QVector<TimelineModel::RenderClip>{ rc });
        }
        return fr;
    };
    // Renderiza un ExportJob y verifica que produce archivo. Devuelve el tamaño (0 si falla).
    auto renderCheck = [&](ExportJob job, const char *label) -> qint64 {
        QFile::remove(job.path);
        ExportWorker worker;
        bool ok = false; QString msg;
        QObject::connect(&worker, &ExportWorker::finished, [&](bool o, const QString &m) { ok = o; msg = m; });
        worker.run(job);
        const qint64 sz = QFileInfo(job.path).size();
        const bool fileOk = ok && sz > 1024;
        check(fileOk, label);
        if (!fileOk && !msg.isEmpty()) qWarning("[CODEC selftest]   %s: %s", label, qUtf8Printable(msg));
        QFile::remove(job.path);
        return fileOk ? sz : 0;
    };

    for (const FmtDesc &f : kFormats) {
        if (!avcodec_find_encoder_by_name(f.venc)) {
            qInfo("[CODEC selftest] %-8s encoder no disponible (omitido)", f.id);
            continue;
        }
        ExportJob job;
        job.path = QDir(QDir::tempPath()).filePath(
            QStringLiteral("pvs_codec_%1.%2").arg(QLatin1String(f.id), QLatin1String(f.ext)));
        job.width = W; job.height = H; job.fps = fps; job.durationUs = 1'000'000;
        job.videoBitrate = 8'000'000; job.format = QString::fromLatin1(f.id);
        for (int i = 0; i < 24; ++i) {
            TimelineModel::RenderClip rc;
            rc.trackIndex = 0; rc.kind = QStringLiteral("video");
            rc.fill = (i % 2) ? QStringLiteral("#3060c0") : QStringLiteral("#c06030");
            rc.sourceUs = 0;
            job.frames.push_back(QVector<TimelineModel::RenderClip>{ rc });
        }
        job.audioS16 = audio;

        QFile::remove(job.path);
        ExportWorker worker;
        bool ok = false; QString msg;
        QObject::connect(&worker, &ExportWorker::finished,
                         [&](bool o, const QString &m) { ok = o; msg = m; });
        worker.run(job);
        const bool fileOk = ok && QFileInfo(job.path).size() > 1024;
        check(fileOk, f.id);
        if (!fileOk && !msg.isEmpty())
            qWarning("[CODEC selftest]   %s: %s", f.id, qUtf8Printable(msg));
        QFile::remove(job.path);
    }

    // Audio mono (downmix) y modo CRF sobre H.264 → ambos producen archivo válido.
    {
        ExportJob mono;
        mono.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_mono.mp4"));
        mono.width = W; mono.height = H; mono.fps = fps; mono.durationUs = 1'000'000;
        mono.videoBitrate = 8'000'000; mono.format = QStringLiteral("h264");
        mono.frames = makeFrames(); mono.audioS16 = audio; mono.channels = 1;
        renderCheck(mono, "mono");

        ExportJob crf;
        crf.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_crf.mp4"));
        crf.width = W; crf.height = H; crf.fps = fps; crf.durationUs = 1'000'000;
        crf.format = QStringLiteral("h264"); crf.useCrf = true; crf.crf = 23;
        crf.frames = makeFrames(); crf.audioS16 = audio;
        renderCheck(crf, "crf");

        ExportJob s51;   // 5.1 (upmix) sobre AAC/MP4
        s51.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_51.mp4"));
        s51.width = W; s51.height = H; s51.fps = fps; s51.durationUs = 1'000'000;
        s51.videoBitrate = 8'000'000; s51.format = QStringLiteral("h264");
        s51.frames = makeFrames(); s51.audioS16 = audio; s51.channels = 6;
        renderCheck(s51, "5.1");

        ExportJob tp;    // 2 pasadas sobre H.264 (bitrate)
        tp.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_2pass.mp4"));
        tp.width = W; tp.height = H; tp.fps = fps; tp.durationUs = 1'000'000;
        tp.videoBitrate = 8'000'000; tp.format = QStringLiteral("h264");
        tp.frames = makeFrames(); tp.audioS16 = audio; tp.twoPass = true;
        renderCheck(tp, "2pass");

        ExportJob s71;   // 7.1 (upmix) sobre AAC/MP4
        s71.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_71.mp4"));
        s71.width = W; s71.height = H; s71.fps = fps; s71.durationUs = 1'000'000;
        s71.videoBitrate = 8'000'000; s71.format = QStringLiteral("h264");
        s71.frames = makeFrames(); s71.audioS16 = audio; s71.channels = 8;
        renderCheck(s71, "7.1");

        ExportJob pf;    // perfil H.264 "high"
        pf.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_prof.mp4"));
        pf.width = W; pf.height = H; pf.fps = fps; pf.durationUs = 1'000'000;
        pf.videoBitrate = 8'000'000; pf.format = QStringLiteral("h264");
        pf.frames = makeFrames(); pf.audioS16 = audio; pf.videoProfile = QStringLiteral("high");
        renderCheck(pf, "perfil");

        ExportJob cr;    // recorte de salida (reencuadre): 20 % por lado, escalado a W×H
        cr.path = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_codec_crop.mp4"));
        cr.width = W; cr.height = H; cr.fps = fps; cr.durationUs = 1'000'000;
        cr.videoBitrate = 8'000'000; cr.format = QStringLiteral("h264");
        cr.frames = makeFrames(); cr.audioS16 = audio;
        cr.cropTop = 0.2; cr.cropBottom = 0.2; cr.cropLeft = 0.2; cr.cropRight = 0.2;
        renderCheck(cr, "recorte");
    }

    qWarning("[CODEC selftest] %d OK, %d FALLO => %s", pass, fail, fail == 0 ? "PASS" : "FALLO");
    return fail == 0 ? 0 : 1;
}

int runCopySelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_COPY_SELFTEST"))
        return -1;

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char *what) {
        qInfo("[COPY selftest] %-38s %s", what, ok ? "OK" : "FAIL");
        if (ok) ++pass; else ++fail;
    };

    const int W = 320, H = 180;
    const double fps = 24.0;
    const QString srcPath = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_copy_src.mp4"));
    const QString outPath = QDir(QDir::tempPath()).filePath(QStringLiteral("pvs_copy_out.mp4"));

    // 1) Genera un MP4 de origen (H.264 + AAC) de 3 s, con keyframes cada 0.5 s (gop=12).
    {
        ExportJob src;
        src.path = srcPath;
        src.width = W; src.height = H; src.fps = fps; src.durationUs = 3'000'000;
        src.videoBitrate = 4'000'000; src.format = QStringLiteral("h264");
        for (int i = 0; i < 72; ++i) {   // 3 s × 24 fps
            TimelineModel::RenderClip rc;
            rc.trackIndex = 0; rc.kind = QStringLiteral("video");
            rc.fill = (i % 2) ? QStringLiteral("#3060c0") : QStringLiteral("#c06030");
            rc.sourceUs = 0;
            src.frames.push_back(QVector<TimelineModel::RenderClip>{ rc });
        }
        src.audioS16 = QByteArray(48000 * 2 * 2 * 3, '\0');   // 3 s de silencio S16 estéreo
        QFile::remove(srcPath);
        ExportWorker w; bool ok = false; QString msg;
        QObject::connect(&w, &ExportWorker::finished, [&](bool o, const QString &m) { ok = o; msg = m; });
        w.run(src);
        check(ok && QFileInfo(srcPath).size() > 1024, "origen H.264+AAC creado");
    }

    // 2) Copia directa de un sub-rango [1.0, 2.5] s → ejercita remuxStreamCopy.
    const qint64 srcBytes = QFileInfo(srcPath).size();
    {
        ExportJob job;
        job.path = outPath;
        job.streamCopy = true;
        job.copySrcPath = srcPath;
        job.copySrcStartUs = 1'000'000;
        job.copySrcEndUs = 2'500'000;
        job.format = QStringLiteral("copy");
        QFile::remove(outPath);
        ExportWorker w; bool ok = false; QString msg;
        QObject::connect(&w, &ExportWorker::finished, [&](bool o, const QString &m) { ok = o; msg = m; });
        w.run(job);
        const qint64 sz = QFileInfo(outPath).size();
        check(ok && sz > 1024, "copia directa produce archivo");
        // El recorte (~1.5 s de 3 s) debe pesar bastante menos que el origen completo.
        check(sz > 0 && sz < srcBytes, "salida más pequeña que el origen (recorte)");
        if (!ok && !msg.isEmpty()) qWarning("[COPY selftest]   motivo: %s", qUtf8Printable(msg));
    }

    // 3) Verifica la salida con libavformat: streams, códecs (sin recodificar) y duración.
    {
        AVFormatContext *in = nullptr;
        const QByteArray p = QFile::encodeName(outPath);
        const bool opened = (avformat_open_input(&in, p.constData(), nullptr, nullptr) == 0);
        check(opened, "salida abre como MP4 válido");
        if (opened) {
            avformat_find_stream_info(in, nullptr);
            bool videoH264 = false, hasAudio = false;
            for (unsigned i = 0; i < in->nb_streams; ++i) {
                const AVCodecParameters *cp = in->streams[i]->codecpar;
                if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_H264
                    && cp->width == W && cp->height == H)
                    videoH264 = true;
                if (cp->codec_type == AVMEDIA_TYPE_AUDIO)
                    hasAudio = true;
            }
            check(videoH264, "vídeo H.264 320×180 copiado sin recodificar");
            check(hasAudio, "pista de audio copiada");
            const double durS = in->duration > 0 ? in->duration / double(AV_TIME_BASE) : 0.0;
            // Rango pedido 1.5 s; el corte arranca en el keyframe <= 1.0 s → 1.0–2.3 s.
            check(durS >= 1.0 && durS <= 2.5, "duración ~1.5 s (corte en keyframe)");
            qInfo("[COPY selftest]   duración salida: %.2f s (rango pedido 1.50 s)", durS);
            avformat_close_input(&in);
        }
    }

    QFile::remove(srcPath);
    QFile::remove(outPath);
    qWarning("[COPY selftest] %d OK, %d FALLO => %s", pass, fail, fail == 0 ? "PASS" : "FALLO");
    return fail == 0 ? 0 : 1;
}
