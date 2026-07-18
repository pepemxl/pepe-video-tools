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
#include <string>

#include "audioengine.h" // AudioMixClip + AudioPlayer (horneado de la mezcla maestra)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
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
};
const FmtDesc kFormats[] = {
    { "h264",     "H.264 · MP4",        "mp4",  "libx264",    AV_PIX_FMT_YUV420P,     nullptr,     true,  "aac",       AV_SAMPLE_FMT_FLTP },
    { "h265",     "H.265/HEVC · MP4",   "mp4",  "libx265",    AV_PIX_FMT_YUV420P,     nullptr,     true,  "aac",       AV_SAMPLE_FMT_FLTP },
    { "prores_lt","ProRes 422 LT · MOV","mov",  "prores_ks",  AV_PIX_FMT_YUV422P10LE, "lt",        false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "prores",   "ProRes 422 · MOV",   "mov",  "prores_ks",  AV_PIX_FMT_YUV422P10LE, "standard",  false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "prores_hq","ProRes 422 HQ · MOV","mov",  "prores_ks",  AV_PIX_FMT_YUV422P10LE, "hq",        false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "prores4444","ProRes 4444 · MOV", "mov",  "prores_ks",  AV_PIX_FMT_YUV444P10LE, "4444",      false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "dnxhr_sq", "DNxHR SQ · MOV",     "mov",  "dnxhd",      AV_PIX_FMT_YUV422P,     "dnxhr_sq",  false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "dnxhr",    "DNxHR HQ · MOV",     "mov",  "dnxhd",      AV_PIX_FMT_YUV422P,     "dnxhr_hq",  false, "pcm_s16le", AV_SAMPLE_FMT_S16 },
    { "vp9",      "VP9 · WebM",         "webm", "libvpx-vp9", AV_PIX_FMT_YUV420P,     nullptr,     true,  "libopus",   AV_SAMPLE_FMT_S16 },
};
const FmtDesc &fmtById(const QString &id)
{
    for (const auto &f : kFormats)
        if (id == QLatin1String(f.id)) return f;
    return kFormats[0];   // por defecto H.264
}

} // namespace

void ExportWorker::run(const ExportJob &job)
{
    auto fail = [this](const QString &m) { emit finished(false, m); };

    const int W = job.width, H = job.height;
    const int frameCount = job.frames.size();
    if (frameCount == 0) { fail(QStringLiteral("Nada que exportar (sin fotogramas).")); return; }

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

    AVStream *vst = avformat_new_stream(oc, nullptr);
    AVCodecContext *vc = avcodec_alloc_context3(vcodec);
    vc->width = W; vc->height = H;
    vc->pix_fmt = fmt.pix;
    vc->time_base = av_d2q(1.0 / job.fps, 1000000);
    vc->framerate = av_d2q(job.fps, 1000000);
    vc->gop_size = 12;
    if (fmt.useBitrate)
        vc->bit_rate = job.videoBitrate;   // ProRes/DNxHR: la calidad la fija el perfil
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // Ajustes de velocidad/calidad por familia de encoder.
    if (fmt.vprofile)
        av_opt_set(vc->priv_data, "profile", fmt.vprofile, 0);   // prores_ks / dnxhd
    if (qstrcmp(fmt.venc, "libx264") == 0 || qstrcmp(fmt.venc, "libx265") == 0)
        av_opt_set(vc->priv_data, "preset", "medium", 0);
    if (qstrcmp(fmt.venc, "libvpx-vp9") == 0) {
        av_opt_set(vc->priv_data, "deadline", "good", 0);
        av_opt_set_int(vc->priv_data, "cpu-used", 4, 0);
    }
    if (avcodec_open2(vc, vcodec, nullptr) < 0) {
        avcodec_free_context(&vc); avformat_free_context(oc);
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
            av_channel_layout_default(&ac->ch_layout, 2);
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

    // --- Conversión de color RGBA → pix_fmt del encoder ---
    SwsContext *sws = sws_getContext(W, H, AV_PIX_FMT_RGBA, W, H, fmt.pix,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    AVFrame *vframe = av_frame_alloc();
    vframe->format = fmt.pix; vframe->width = W; vframe->height = H;
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
        av_channel_layout_default(&aframe->ch_layout, 2);
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
            if (ac->sample_fmt == AV_SAMPLE_FMT_S16) {
                // Empaquetado intercalado L R L R (PCM/Opus).
                auto *d = reinterpret_cast<int16_t *>(aframe->data[0]);
                for (int k = 0; k < n; ++k) {
                    d[2 * k]     = int16_t(std::clamp(int(std::lround(aL[aStart + k] * 32767.0f)), -32768, 32767));
                    d[2 * k + 1] = int16_t(std::clamp(int(std::lround(aR[aStart + k] * 32767.0f)), -32768, 32767));
                }
            } else {
                // Planar float (AAC): un plano por canal.
                std::memcpy(aframe->data[0], aL.constData() + aStart, n * sizeof(float));
                std::memcpy(aframe->data[1], aR.constData() + aStart, n * sizeof(float));
            }
            aframe->pts = aStart;
            if (avcodec_send_frame(ac, aframe) < 0) return false;
            if (!drainEncoder(oc, ac, ast, pkt)) return false;
            aStart += n;
        }
        return true;
    };

    // --- Bucle principal: componer + codificar cada fotograma ---
    bool ok = true;
    QImage black(W, H, QImage::Format_RGBA8888);
    black.fill(Qt::black);
    for (int i = 0; i < frameCount && ok; ++i) {
        bool hasContent = false;
        QImage img = comp.renderFrame(job.frames[i], hasContent);
        if (img.isNull() || img.size() != QSize(W, H))
            img = black;
        if (img.format() != QImage::Format_RGBA8888)
            img = img.convertToFormat(QImage::Format_RGBA8888);

        if (av_frame_make_writable(vframe) < 0) { ok = false; break; }
        const uint8_t *srcData[4] = { img.constBits(), nullptr, nullptr, nullptr };
        int srcStride[4] = { int(img.bytesPerLine()), 0, 0, 0 };
        sws_scale(sws, srcData, srcStride, 0, H, vframe->data, vframe->linesize);
        vframe->pts = i;
        if (avcodec_send_frame(vc, vframe) < 0) { ok = false; break; }
        if (!drainEncoder(oc, vc, vst, pkt)) { ok = false; break; }

        // Audio hasta el final de este fotograma (mantiene A/V intercalados).
        if (!encodeAudioUntil(double(i + 1) / job.fps)) { ok = false; break; }

        if ((i & 7) == 0 || i == frameCount - 1)
            emit progress(double(i + 1) / frameCount);
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

    if (ok) emit finished(true, job.path);
    else    emit finished(false, QStringLiteral("Fallo durante la codificación."));
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

    // Instantánea de vídeo: RenderClipList por fotograma, desde startUs (rango I/O).
    const int frameCount = int(std::ceil((durUs / 1e6) * fps));
    job.frames.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        const qint64 us = startUs + qint64((double(i) / fps) * 1e6);
        job.frames.push_back(m_timeline->clipsAt(us));
    }

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
    if (!buildJob(path, width, height, fps, m_mbps, m_audioKbps, startUs, durUs, job)) {
        setStatus(QStringLiteral("El rango de exportación está vacío.")); return;
    }
    job.format = m_format;

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
        if (avcodec_find_encoder_by_name(f.venc))
            out.append(QVariantMap{ { "id", QString::fromLatin1(f.id) },
                                    { "label", QString::fromLatin1(f.label) },
                                    { "ext", QString::fromLatin1(f.ext) } });
    return out;
}

void Exporter::setFormat(const QString &id)
{
    if (id == m_format) return;
    bool ok = false;   // solo formatos cuyo encoder existe en este build
    for (const auto &f : kFormats)
        if (id == QLatin1String(f.id) && avcodec_find_encoder_by_name(f.venc)) { ok = true; break; }
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
    if (!buildJob(it.path, it.width, it.height, it.fps, it.mbps, it.audioKbps, it.startUs, it.durUs, job)) {
        it.status = 3;   // error (rango vacío)
        emit queueChanged();
        renderNextInQueue();
        return;
    }
    job.format = it.format;
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

    qWarning("[CODEC selftest] %d OK, %d FALLO => %s", pass, fail, fail == 0 ? "PASS" : "FALLO");
    return fail == 0 ? 0 : 1;
}
