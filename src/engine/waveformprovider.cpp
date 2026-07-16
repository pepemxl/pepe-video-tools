#include "waveformprovider.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// ---------------------------------------------------------------------------
// WaveformWorker
// ---------------------------------------------------------------------------

// Recorre todo el audio del archivo (remuestreado a mono S16 · 48 kHz) y guarda
// el pico absoluto de cada hop de 10 ms. Sin ganancia/pan: la envolvente es del
// origen, así que sirve para cualquier clip que use el archivo (slip/trim/speed
// solo cambian qué ventana se consulta).
QVector<float> WaveformWorker::analyzeFile(const QString &path)
{
    QVector<float> env;
    if (path.isEmpty())
        return env;

    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    SwrContext *swr = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    auto cleanup = [&]() {
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    };

    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return env;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { cleanup(); return env; }

    const AVCodec *dec = nullptr;
    const int audioIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (audioIndex < 0 || !dec) { cleanup(); return env; }

    codec = avcodec_alloc_context3(dec);
    if (!codec ||
        avcodec_parameters_to_context(codec, fmt->streams[audioIndex]->codecpar) < 0 ||
        avcodec_open2(codec, dec, nullptr) < 0) { cleanup(); return env; }

    AVChannelLayout mono;
    av_channel_layout_default(&mono, 1);
    const int rc = swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, kOutRate,
                                       &codec->ch_layout, codec->sample_fmt,
                                       codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&mono);
    if (rc < 0 || !swr || swr_init(swr) < 0) { cleanup(); return env; }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();

    // Cota de memoria: 4 h de envolvente (~1.4 M floats) bastan de sobra.
    constexpr qint64 kMaxEnv = 4LL * 3600 * kEnvPerSec;
    int hopFill = 0;
    float hopPeak = 0.0f;
    auto consume = [&](const int16_t *s, int n) {
        for (int i = 0; i < n; ++i) {
            const float a = std::abs(int(s[i])) / 32768.0f;
            if (a > hopPeak) hopPeak = a;
            if (++hopFill == kEnvHop) {
                env.push_back(hopPeak);
                hopPeak = 0.0f;
                hopFill = 0;
            }
        }
    };

    bool draining = false;
    while (env.size() < kMaxEnv) {
        const int ret = avcodec_receive_frame(codec, frame);
        if (ret == 0) {
            const int outSamples = int(av_rescale_rnd(
                swr_get_delay(swr, codec->sample_rate) + frame->nb_samples,
                kOutRate, codec->sample_rate, AV_ROUND_UP));
            uint8_t *buf = nullptr;
            if (av_samples_alloc(&buf, nullptr, 1, outSamples, AV_SAMPLE_FMT_S16, 0) >= 0) {
                const int got = swr_convert(swr, &buf, outSamples,
                                            (const uint8_t **)frame->data, frame->nb_samples);
                if (got > 0)
                    consume(reinterpret_cast<const int16_t *>(buf), got);
                av_freep(&buf);
            }
            av_frame_unref(frame);
            continue;
        }
        if (ret != AVERROR(EAGAIN) || draining)
            break;  // AVERROR_EOF (drenaje completo) o error de decodificación
        if (av_read_frame(fmt, pkt) < 0) {
            avcodec_send_packet(codec, nullptr);  // fin de archivo: drenar el códec
            draining = true;
            continue;
        }
        if (pkt->stream_index == audioIndex)
            avcodec_send_packet(codec, pkt);
        av_packet_unref(pkt);
    }
    if (hopFill > 0)
        env.push_back(hopPeak);

    cleanup();
    return env;
}

void WaveformWorker::analyze(const QString &path)
{
    emit envelopeReady(path, analyzeFile(path));
}

// ---------------------------------------------------------------------------
// WaveformProvider
// ---------------------------------------------------------------------------

WaveformProvider::WaveformProvider(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<QVector<float>>("QVector<float>");
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("waveforms"));
    m_worker = new WaveformWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &WaveformProvider::requestAnalyze, m_worker, &WaveformWorker::analyze);
    connect(m_worker, &WaveformWorker::envelopeReady, this,
            [this](const QString &path, const QVector<float> &envelope) {
                m_pending.remove(path);
                m_cache.insert(path, envelope);  // vacía también: no reintentar sin audio
                if (!qEnvironmentVariableIsEmpty("PVS_WAVE_DEBUG"))
                    qInfo("[WAVE] envolvente lista: %s (%lld valores)",
                          qUtf8Printable(path), qint64(envelope.size()));
                if (!envelope.isEmpty())
                    emit ready(path);
            });
    m_thread->start();
}

WaveformProvider::~WaveformProvider()
{
    m_thread->quit();
    m_thread->wait();
}

QVariantList WaveformProvider::peaks(const QString &path, double inSec, double durSec,
                                     double speed, int bins)
{
    QVariantList out;
    if (path.isEmpty() || bins <= 0 || durSec <= 0.0)
        return out;

    const auto it = m_cache.constFind(path);
    if (it == m_cache.constEnd()) {
        if (!m_pending.contains(path)) {
            m_pending.insert(path);
            emit requestAnalyze(path);
        }
        return out;
    }
    const QVector<float> &env = it.value();
    if (env.isEmpty())
        return out;  // el archivo no tiene pista de audio

    if (speed <= 0.0)
        speed = 1.0;
    // Ventana de origen del clip en índices de envolvente, repartida en `bins`
    // columnas: cada columna toma el pico de su sub-rango (fuera de rango = 0).
    const double e0 = inSec * WaveformWorker::kEnvPerSec;
    const double perBin = (durSec * speed * WaveformWorker::kEnvPerSec) / bins;
    out.reserve(bins);
    for (int b = 0; b < bins; ++b) {
        const int i0 = int(e0 + b * perBin);
        const int i1 = qMax(i0 + 1, int(e0 + (b + 1) * perBin));
        float pk = 0.0f;
        for (int i = qMax(0, i0); i < qMin(i1, int(env.size())); ++i)
            if (env[i] > pk) pk = env[i];
        out.append(pk);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Auto-test (PVS_WAVE_SELFTEST)
// ---------------------------------------------------------------------------

namespace {

// WAV PCM S16 mono a 48 kHz: primera mitad silencio, segunda mitad tono de
// 1 kHz con amplitud `amp`. Permite verificar nivel Y posición en la envolvente.
bool writeHalfToneWav(const QString &path, double amp, double seconds)
{
    constexpr double kPi = 3.14159265358979323846;
    const int rate = 48000;
    const int frames = int(rate * seconds);
    QByteArray pcm(frames * 2, 0);
    auto *s = reinterpret_cast<int16_t *>(pcm.data());
    for (int i = frames / 2; i < frames; ++i)
        s[i] = int16_t(std::lround(amp * 32767.0 * std::sin(2.0 * kPi * 1000.0 * i / rate)));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const quint32 dataSize = quint32(pcm.size());
    auto putU32 = [&f](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto putU16 = [&f](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF"); putU32(36 + dataSize); f.write("WAVE");
    f.write("fmt "); putU32(16); putU16(1); putU16(1);            // PCM, mono
    putU32(rate); putU32(rate * 2); putU16(2); putU16(16);        // byteRate, block, bits
    f.write("data"); putU32(dataSize);
    f.write(pcm);
    return true;
}

double listMax(const QVariantList &l, int from, int to)
{
    double mx = 0.0;
    for (int i = qMax(0, from); i < qMin(to, int(l.size())); ++i)
        mx = qMax(mx, l.at(i).toDouble());
    return mx;
}

} // namespace

int runWaveformSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_WAVE_SELFTEST"))
        return -1;

    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        qInfo("[WAVE-SELFTEST] %-52s %s", name, ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    };

    const QString wav = QDir::tempPath() + "/pvs_wave.wav";
    check(writeHalfToneWav(wav, 0.8, 1.0), "genera WAV mitad silencio / mitad tono");

    // 1) Análisis directo (sincrónico).
    const QVector<float> env = WaveformWorker::analyzeFile(wav);
    check(std::abs(env.size() - 100) <= 2, "envolvente de 1 s ~= 100 valores");
    float silPk = 0.0f, tonePk = 0.0f;
    for (int i = 0; i < qMin(45, int(env.size())); ++i) silPk = qMax(silPk, env[i]);
    for (int i = 55; i < qMin(95, int(env.size())); ++i) tonePk = qMax(tonePk, env[i]);
    check(silPk < 0.02f, "mitad de silencio ~= 0");
    check(tonePk > 0.7f && tonePk <= 0.85f, "mitad de tono ~= amplitud (0.8)");
    check(WaveformWorker::analyzeFile(QDir::tempPath() + "/pvs_no_existe.wav").isEmpty(),
          "archivo inexistente -> envolvente vacia");

    // 2) Camino completo del proveedor: encolar → hilo → caché → ready → peaks().
    WaveformProvider prov;
    check(prov.peaks(wav, 0.0, 1.0, 1.0, 50).isEmpty(), "primera consulta encola (sin datos)");
    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    QObject::connect(&prov, &WaveformProvider::ready, &loop, &QEventLoop::quit);
    loop.exec();
    const QVariantList p = prov.peaks(wav, 0.0, 1.0, 1.0, 50);
    check(p.size() == 50, "tras ready(): peaks() devuelve las 50 columnas");
    check(listMax(p, 0, 20) < 0.02, "columnas de silencio ~= 0");
    check(listMax(p, 30, 48) > 0.7, "columnas del tono ~= amplitud");
    // Ventana desplazada (slip): consultar solo la mitad con tono.
    check(listMax(prov.peaks(wav, 0.5, 0.5, 1.0, 10), 0, 10) > 0.7,
          "in-point 0.5 s -> ventana con tono");
    // Velocidad 2×: 0.5 s de timeline cubren 1 s de origen (incluye el tono).
    check(listMax(prov.peaks(wav, 0.0, 0.5, 2.0, 10), 5, 10) > 0.7,
          "velocidad 2x amplia la ventana de origen");

    qInfo("[WAVE-SELFTEST] resultado: %s (%d fallos)", failures ? "FALLO" : "OK", failures);
    return failures ? 1 : 0;
}
