#include "exporter.h"
#include "compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QThread>
#include <cmath>
#include <cstring>

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

    // --- Encoder de vídeo (H.264 / libx264) ---
    const AVCodec *vcodec = avcodec_find_encoder_by_name("libx264");
    if (!vcodec) vcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vcodec) { avformat_free_context(oc); fail(QStringLiteral("Encoder H.264 no disponible.")); return; }

    AVStream *vst = avformat_new_stream(oc, nullptr);
    AVCodecContext *vc = avcodec_alloc_context3(vcodec);
    vc->width = W; vc->height = H;
    vc->pix_fmt = AV_PIX_FMT_YUV420P;
    vc->time_base = av_d2q(1.0 / job.fps, 1000000);
    vc->framerate = av_d2q(job.fps, 1000000);
    vc->gop_size = 12;
    vc->bit_rate = job.videoBitrate;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(vc->priv_data, "preset", "medium", 0);
    if (avcodec_open2(vc, vcodec, nullptr) < 0) {
        avcodec_free_context(&vc); avformat_free_context(oc);
        fail(QStringLiteral("No se pudo abrir el encoder de vídeo.")); return;
    }
    avcodec_parameters_from_context(vst->codecpar, vc);
    vst->time_base = vc->time_base;

    // --- Encoder de audio (AAC), opcional si hay mezcla ---
    const bool hasAudio = !job.audioS16.isEmpty();
    AVStream *ast = nullptr;
    AVCodecContext *ac = nullptr;
    if (hasAudio) {
        const AVCodec *acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (acodec) {
            ast = avformat_new_stream(oc, nullptr);
            ac = avcodec_alloc_context3(acodec);
            ac->sample_fmt = AV_SAMPLE_FMT_FLTP;
            ac->sample_rate = 48000;
            av_channel_layout_default(&ac->ch_layout, 2);
            ac->bit_rate = job.audioBitrate;
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

    // --- Conversión de color RGBA → YUV420P ---
    SwsContext *sws = sws_getContext(W, H, AV_PIX_FMT_RGBA, W, H, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    AVFrame *vframe = av_frame_alloc();
    vframe->format = AV_PIX_FMT_YUV420P; vframe->width = W; vframe->height = H;
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
        aframe->format = AV_SAMPLE_FMT_FLTP;
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
            std::memcpy(aframe->data[0], aL.constData() + aStart, n * sizeof(float));
            std::memcpy(aframe->data[1], aR.constData() + aStart, n * sizeof(float));
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
        setStatus(ok ? QStringLiteral("Exportado: %1").arg(QFileInfo(msg).fileName())
                     : QStringLiteral("Error: %1").arg(msg));
        emit progressChanged();
        emit runningChanged();
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

void Exporter::exportTimeline(const QString &path, int width, int height,
                              double fps, double seconds)
{
    if (m_running || !m_timeline || path.isEmpty()) return;
    if (width <= 0 || height <= 0 || fps <= 0) return;

    // Duración: por defecto, todo el contenido de la línea de tiempo.
    qint64 durUs = seconds > 0 ? qint64(seconds * 1e6) : m_timeline->contentEndUs();
    if (durUs <= 0) { setStatus(QStringLiteral("La línea de tiempo está vacía.")); return; }

    ExportJob job;
    job.path = path;
    job.width = width; job.height = height; job.fps = fps;
    job.durationUs = durUs;

    // Instantánea de vídeo: RenderClipList por fotograma (en el hilo de GUI, seguro).
    const int frameCount = int(std::ceil(seconds > 0 ? seconds * fps : (durUs / 1e6) * fps));
    job.frames.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        const qint64 us = qint64((double(i) / fps) * 1e6);
        job.frames.push_back(m_timeline->clipsAt(us));
    }

    // Instantánea de audio: hornea la mezcla maestra con un AudioPlayer temporal.
    QVector<AudioMixClip> mix;
    for (const TimelineModel::AudioClip &a : m_timeline->audioClips()) {
        AudioMixClip m;
        m.mediaPath = a.mediaPath; m.trackIndex = a.trackIndex;
        m.startUs = a.startUs; m.durationUs = a.durationUs; m.inUs = a.inUs;
        m.speed = a.speed; m.gain = a.gain; m.pan = a.pan; m.mute = a.mute;
        for (const TimelineModel::Keyframe &k : a.gainKf) m.gainKf.push_back({ k.sourceUs, k.value });
        for (const TimelineModel::Keyframe &k : a.panKf) m.panKf.push_back({ k.sourceUs, k.value });
        mix.push_back(m);
    }
    {
        AudioPlayer baker;
        baker.setMix(mix, durUs);
        job.audioS16 = baker.master();
    }

    m_running = true; m_progress = 0.0;
    setStatus(QStringLiteral("Exportando…"));
    emit runningChanged();
    emit progressChanged();
    emit requestRun(job);
}

void Exporter::openExportDialog()
{
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
        exportTimeline(path, 1920, 1080, 30.0, 0.0);
    }
#else
    setStatus(QStringLiteral("Diálogo de exportación solo disponible en Windows."));
#endif
}

// ============================ Auto-test ============================

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
    const bool pass = done && ok && exists;
    qInfo("[EXPORT selftest] ok=%d archivo=%s tam=%lld  => %s",
          int(ok), qUtf8Printable(job.path), exists ? fi.size() : -1,
          pass ? "PASS" : "FALLO");
    return pass ? 0 : 1;
}
