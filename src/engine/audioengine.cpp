#include "audioengine.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {

// Interpola linealmente una automatización de ganancia (sourceUs, valor) en sourceUs.
double evalGainKf(const QVector<QPair<qint64, double>> &kf, double staticVal, qint64 sourceUs)
{
    if (kf.isEmpty()) return staticVal;
    if (sourceUs <= kf.first().first) return kf.first().second;
    if (sourceUs >= kf.last().first) return kf.last().second;
    for (int i = 1; i < kf.size(); ++i) {
        if (sourceUs <= kf[i].first) {
            const auto &a = kf[i - 1];
            const auto &b = kf[i];
            const double span = double(b.first - a.first);
            const double t = span > 0 ? double(sourceUs - a.first) / span : 0.0;
            return a.second + (b.second - a.second) * t;
        }
    }
    return kf.last().second;
}

// Biquad directo forma II transpuesta para el filtrado K-weighting.
struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1 = 0.0, z2 = 0.0;
    double process(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Coeficientes RBJ (Audio EQ Cookbook), ya normalizados por a0, para el EQ de pista.
constexpr double kPi = 3.14159265358979323846;

Biquad makeLowShelf(double fs, double f0, double dB)
{
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs, cs = std::cos(w0), sn = std::sin(w0);
    const double beta = 2.0 * std::sqrt(A) * (sn / 2.0 * std::sqrt(2.0)); // S = 1
    const double b0 = A * ((A + 1) - (A - 1) * cs + beta);
    const double b1 = 2 * A * ((A - 1) - (A + 1) * cs);
    const double b2 = A * ((A + 1) - (A - 1) * cs - beta);
    const double a0 = (A + 1) + (A - 1) * cs + beta;
    const double a1 = -2 * ((A - 1) + (A + 1) * cs);
    const double a2 = (A + 1) + (A - 1) * cs - beta;
    return Biquad{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

Biquad makeHighShelf(double fs, double f0, double dB)
{
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs, cs = std::cos(w0), sn = std::sin(w0);
    const double beta = 2.0 * std::sqrt(A) * (sn / 2.0 * std::sqrt(2.0)); // S = 1
    const double b0 = A * ((A + 1) + (A - 1) * cs + beta);
    const double b1 = -2 * A * ((A - 1) + (A + 1) * cs);
    const double b2 = A * ((A + 1) + (A - 1) * cs - beta);
    const double a0 = (A + 1) - (A - 1) * cs + beta;
    const double a1 = 2 * ((A - 1) - (A + 1) * cs);
    const double a2 = (A + 1) - (A - 1) * cs - beta;
    return Biquad{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

Biquad makePeak(double fs, double f0, double Q, double dB)
{
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs, cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * Q);
    const double b0 = 1 + alpha * A, b1 = -2 * cs, b2 = 1 - alpha * A;
    const double a0 = 1 + alpha / A, a1 = -2 * cs, a2 = 1 - alpha / A;
    return Biquad{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

} // namespace

// ---------------------------------------------------------------------------
// AudioPlayer (hilo worker)
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent) {}

AudioPlayer::~AudioPlayer()
{
    stop();
    closeSource();
}

bool AudioPlayer::openSource(const QString &path)
{
    closeSource();
    if (path.isEmpty())
        return false;
    if (avformat_open_input(&m_fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        m_fmt = nullptr;
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) { closeSource(); return false; }

    const AVCodec *dec = nullptr;
    m_audioIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (m_audioIndex < 0 || !dec) { closeSource(); return false; }

    m_codec = avcodec_alloc_context3(dec);
    if (!m_codec) { closeSource(); return false; }
    AVStream *st = m_fmt->streams[m_audioIndex];
    if (avcodec_parameters_to_context(m_codec, st->codecpar) < 0 ||
        avcodec_open2(m_codec, dec, nullptr) < 0) { closeSource(); return false; }
    m_timeBaseSec = av_q2d(st->time_base);

    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    m_eof = false;
    if (!ensureSwr()) { closeSource(); return false; }
    return true;
}

bool AudioPlayer::ensureSwr()
{
    if (m_swr) return true;
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2);
    int rc = swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_S16, kOutRate,
                                 &m_codec->ch_layout, m_codec->sample_fmt, m_codec->sample_rate,
                                 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (rc < 0 || !m_swr) return false;
    return swr_init(m_swr) >= 0;
}

void AudioPlayer::closeSource()
{
    if (m_swr) { swr_free(&m_swr); m_swr = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_pkt) { av_packet_free(&m_pkt); m_pkt = nullptr; }
    if (m_codec) { avcodec_free_context(&m_codec); m_codec = nullptr; }
    if (m_fmt) { avformat_close_input(&m_fmt); m_fmt = nullptr; }
    m_audioIndex = -1;
    m_eof = false;
}

void AudioPlayer::seekMs(qint64 ms)
{
    if (!m_fmt || m_audioIndex < 0) return;
    const qint64 ts = m_timeBaseSec > 0.0 ? qint64((ms / 1000.0) / m_timeBaseSec) : 0;
    av_seek_frame(m_fmt, m_audioIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (m_codec) avcodec_flush_buffers(m_codec);
    m_eof = false;
}

// Decodifica un bloque de audio como PCM S16 estéreo intercalado (48 kHz), sin ganancia.
QByteArray AudioPlayer::decodeChunk(double &peakL, double &peakR)
{
    peakL = peakR = 0.0;
    QByteArray out;
    if (!m_fmt || !m_codec || !m_swr || m_eof) return out;

    int maxL = 0, maxR = 0;
    while (out.isEmpty() && !m_eof) {
        int ret = avcodec_receive_frame(m_codec, m_frame);
        if (ret == 0) {
            const int inSamples = m_frame->nb_samples;
            const int outSamples = int(av_rescale_rnd(
                swr_get_delay(m_swr, m_codec->sample_rate) + inSamples,
                kOutRate, m_codec->sample_rate, AV_ROUND_UP));
            uint8_t *buf = nullptr;
            if (av_samples_alloc(&buf, nullptr, 2, outSamples, AV_SAMPLE_FMT_S16, 0) < 0) {
                av_frame_unref(m_frame); continue;
            }
            const int got = swr_convert(m_swr, &buf, outSamples,
                                        (const uint8_t **)m_frame->data, inSamples);
            av_frame_unref(m_frame);
            if (got > 0) {
                const int n = got * 2;
                auto *s = reinterpret_cast<int16_t *>(buf);
                for (int i = 0; i < n; ++i) {
                    const int a = std::abs(int(s[i]));
                    if (i & 1) { if (a > maxR) maxR = a; }
                    else       { if (a > maxL) maxL = a; }
                }
                out.append(reinterpret_cast<const char *>(buf), n * int(sizeof(int16_t)));
            }
            av_freep(&buf);
            continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            int rd = av_read_frame(m_fmt, m_pkt);
            if (rd < 0) {
                avcodec_send_packet(m_codec, nullptr);
                if (avcodec_receive_frame(m_codec, m_frame) != 0) m_eof = true;
                else av_frame_unref(m_frame);
                continue;
            }
            if (m_pkt->stream_index == m_audioIndex)
                avcodec_send_packet(m_codec, m_pkt);
            av_packet_unref(m_pkt);
            continue;
        }
        m_eof = true;
    }
    peakL = maxL / 32768.0;
    peakR = maxR / 32768.0;
    return out;
}

QVector<float> AudioPlayer::decodeClipFloat(const AudioMixClip &clip)
{
    QVector<float> out;
    if (!openSource(clip.mediaPath))
        return out;
    seekMs(clip.inUs / 1000);

    // Muestras de origen a recolectar para cubrir la duración de salida × velocidad.
    const qint64 wantFrames = qint64((clip.durationUs / 1e6) * clip.speed * kOutRate);
    QVector<float> src;
    src.reserve(int(qMin<qint64>(wantFrames + 4096, 1 << 26)) * 2);
    qint64 haveFrames = 0;
    while (haveFrames < wantFrames) {
        double pL, pR;
        QByteArray chunk = decodeChunk(pL, pR);
        if (chunk.isEmpty()) break;
        const int n = chunk.size() / int(sizeof(int16_t)); // muestras int16
        const auto *s = reinterpret_cast<const int16_t *>(chunk.constData());
        for (int i = 0; i < n; ++i) src.push_back(s[i] / 32768.0f);
        haveFrames += n / 2;
    }
    closeSource();

    const qint64 outSamples = qint64((clip.durationUs / 1e6) * kOutRate);
    const qint64 srcFrames = src.size() / 2;
    if (outSamples <= 0 || srcFrames <= 0) return out;
    out.resize(int(outSamples * 2));
    for (qint64 k = 0; k < outSamples; ++k) {
        const double pos = k * clip.speed;      // posición (frames) en el origen a 48 kHz
        const qint64 i0 = qint64(pos);
        const double f = pos - double(i0);
        for (int ch = 0; ch < 2; ++ch) {
            const float a = (i0 < srcFrames) ? src[int(i0 * 2 + ch)] : 0.0f;
            const float b = (i0 + 1 < srcFrames) ? src[int((i0 + 1) * 2 + ch)] : a;
            out[int(k * 2 + ch)] = a + (b - a) * float(f);
        }
    }
    return out;
}

// decodeClipFloat con caché: la parte cara (decodificar+remuestrear) no depende de
// ganancia/pan/mute, así que editar esos parámetros reutiliza el PCM ya decodificado.
QVector<float> AudioPlayer::decodeCached(const AudioMixClip &clip)
{
    const QString key = clip.mediaPath + '|' + QString::number(clip.inUs) + '|'
                        + QString::number(clip.durationUs) + '|'
                        + QString::number(clip.speed, 'f', 4);
    auto it = m_clipCache.constFind(key);
    if (it != m_clipCache.constEnd())
        return it.value();
    if (m_clipCache.size() > 48)      // cota simple de memoria
        m_clipCache.clear();
    QVector<float> pcm = decodeClipFloat(clip);
    m_clipCache.insert(key, pcm);
    return pcm;
}

// Hornea la mezcla: suma todos los clips (ganancia/automatización/pan/velocidad) en un
// master float, guarda la envolvente de pico por pista y convierte el master a S16.
void AudioPlayer::bake(const QVector<AudioMixClip> &clips, qint64 endUs,
                       double masterGain, double masterPan)
{
    m_master.clear();
    m_env.clear();
    m_lufs = -70.0;
    m_cursor = 0;

    if (endUs <= 0)
        for (const auto &c : clips) endUs = qMax(endUs, c.startUs + c.durationUs);
    const qint64 totalSamples = qint64((endUs / 1e6) * kOutRate);
    if (totalSamples <= 0) return;

    QVector<float> mix(int(totalSamples * 2), 0.0f);

    int nTracks = 6; // A1–A3 viven en los índices 3–5
    for (const auto &c : clips) nTracks = qMax(nTracks, c.trackIndex + 1);
    const qint64 envLen = (totalSamples + kEnvHop - 1) / kEnvHop;
    m_env.resize(nTracks);
    for (auto &e : m_env) e.fill(0.0f, int(envLen));

    // Ley de balance: unidad al centro, atenúa el canal opuesto hacia los extremos.
    auto balance = [](double pan, double &gL, double &gR) {
        gL = pan <= 0.0 ? 1.0 : 1.0 - pan;
        gR = pan >= 0.0 ? 1.0 : 1.0 + pan;
    };

    // Agrupa los clips por pista: cada pista se mezcla en un submix propio, se procesa
    // con sus efectos (EQ → compresor) y se suma al master. Así los efectos actúan
    // sobre el bus de la pista, no clip a clip.
    QHash<int, QVector<int>> byTrack;
    for (int i = 0; i < clips.size(); ++i)
        if (!clips[i].mute) byTrack[clips[i].trackIndex].push_back(i);

    QVector<float> sub(int(totalSamples * 2));   // submix de la pista (reutilizado)
    const double fs = double(kOutRate);

    for (auto it = byTrack.constBegin(); it != byTrack.constEnd(); ++it) {
        const int trackIndex = it.key();
        const QVector<int> &idxs = it.value();
        std::fill(sub.begin(), sub.end(), 0.0f);

        // 1) Suma los clips de la pista (ganancia/pan de clip + automatización).
        for (int ci : idxs) {
            const AudioMixClip &c = clips[ci];
            QVector<float> pcm = decodeCached(c);   // reutiliza PCM decodificado si no cambió
            const qint64 outSamples = pcm.size() / 2;
            if (outSamples <= 0) continue;
            const qint64 startSample = qint64((c.startUs / 1e6) * kOutRate);
            double sgL, sgR; balance(c.pan, sgL, sgR); // ganancias estáticas de pan
            const bool hasGainKf = !c.gainKf.isEmpty();
            const bool hasPanKf = !c.panKf.isEmpty();
            const bool needSrc = hasGainKf || hasPanKf;
            for (qint64 k = 0; k < outSamples; ++k) {
                const qint64 idx = startSample + k;
                if (idx < 0 || idx >= totalSamples) continue;
                double g = c.gain, gL = sgL, gR = sgR;
                if (needSrc) {
                    const qint64 srcUs = c.inUs + qint64((k / double(kOutRate)) * 1e6 * c.speed);
                    if (hasGainKf) g = evalGainKf(c.gainKf, c.gain, srcUs);
                    if (hasPanKf) balance(evalGainKf(c.panKf, c.pan, srcUs), gL, gR);
                }
                sub[int(idx * 2)]     += pcm[int(k * 2)] * float(g * gL);
                sub[int(idx * 2 + 1)] += pcm[int(k * 2 + 1)] * float(g * gR);
            }
        }

        // 2) Efectos de la pista (iguales para todos sus clips): EQ de 3 bandas → compresor.
        const AudioMixClip &fx = clips[idxs.first()];
        if (fx.eqOn) {
            Biquad loL = makeLowShelf(fs, 120.0, fx.eqLowDb),  loR = loL;
            Biquad miL = makePeak(fs, 1000.0, 0.9, fx.eqMidDb), miR = miL;
            Biquad hiL = makeHighShelf(fs, 8000.0, fx.eqHighDb), hiR = hiL;
            for (qint64 f = 0; f < totalSamples; ++f) {
                sub[int(f * 2)]     = float(hiL.process(miL.process(loL.process(sub[int(f * 2)]))));
                sub[int(f * 2 + 1)] = float(hiR.process(miR.process(loR.process(sub[int(f * 2 + 1)]))));
            }
        }
        if (fx.compOn) {
            const double thresh = std::pow(10.0, fx.compThreshDb / 20.0);
            const double makeup = std::pow(10.0, fx.compMakeupDb / 20.0);
            const double atk = std::exp(-1.0 / (fs * 0.010));   // 10 ms
            const double rel = std::exp(-1.0 / (fs * 0.100));   // 100 ms
            const double invR = 1.0 / fx.compRatio;
            double envc = 0.0;
            for (qint64 f = 0; f < totalSamples; ++f) {
                const double l = sub[int(f * 2)], r = sub[int(f * 2 + 1)];
                const double sidet = qMax(std::fabs(l), std::fabs(r));
                envc = (sidet > envc ? atk : rel) * envc + (1.0 - (sidet > envc ? atk : rel)) * sidet;
                double gr = 1.0;
                if (envc > thresh && envc > 1e-9)
                    gr = std::pow(envc / thresh, invR - 1.0);   // reducción (<1)
                const double g = gr * makeup;
                sub[int(f * 2)]     = float(l * g);
                sub[int(f * 2 + 1)] = float(r * g);
            }
        }

        // 3) Acumula en el master y calcula la envolvente de la pista (post-efectos).
        QVector<float> &env = m_env[qBound(0, trackIndex, nTracks - 1)];
        for (qint64 f = 0; f < totalSamples; ++f) {
            const float l = sub[int(f * 2)], r = sub[int(f * 2 + 1)];
            mix[int(f * 2)]     += l;
            mix[int(f * 2 + 1)] += r;
            const float a = qMax(std::fabs(l), std::fabs(r));
            float &ev = env[int(f / kEnvHop)];
            if (a > ev) ev = a;
        }
    }

    // Bus MAIN: ganancia + pan del master aplicados al bajar a S16 (post-mezcla).
    double mgL, mgR; balance(masterPan, mgL, mgR);
    const float masterL = float(masterGain * mgL), masterR = float(masterGain * mgR);
    m_master.resize(int(totalSamples * 2 * qint64(sizeof(int16_t))));
    auto *out = reinterpret_cast<int16_t *>(m_master.data());
    for (qint64 f = 0; f < totalSamples; ++f) {
        const int vl = int(std::lround(mix[int(f * 2)] * masterL * 32767.0f));
        const int vr = int(std::lround(mix[int(f * 2 + 1)] * masterR * 32767.0f));
        out[f * 2] = int16_t(std::clamp(vl, -32768, 32767));
        out[f * 2 + 1] = int16_t(std::clamp(vr, -32768, 32767));
    }

    m_lufs = kWeightAnalyze(m_master, kOutRate, kEnvHop, m_lufsEnv);

    if (!qEnvironmentVariableIsEmpty("PVS_AUDIO_DEBUG"))
        qInfo("[AUDIO] horneado: %lld clips, fin=%.1fs, master=%lld frames, LUFS=%.1f",
              qint64(clips.size()), endUs / 1e6, totalSamples, m_lufs);
}

// Filtra el master con K-weighting (BS.1770). Devuelve el LUFS integrado (bloques de 400 ms
// con puerta absoluta) y llena shortEnv con el loudness a corto plazo (ventana de 400 ms)
// por hop de envolvente, para el medidor en vivo.
double AudioPlayer::kWeightAnalyze(const QByteArray &masterS16, int rate, int envHop,
                                   QVector<float> &shortEnv)
{
    shortEnv.clear();
    const qint64 frames = masterS16.size() / 4;
    if (frames <= 0 || envHop <= 0) return -70.0;
    const auto *s = reinterpret_cast<const int16_t *>(masterS16.constData());

    // Coeficientes K-weighting a 48 kHz (etapa shelving + paso-alto).
    Biquad sL{ 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
    Biquad sR = sL;
    Biquad hL{ 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
    Biquad hR = hL;

    // Suma de energía K-weighted por hop (z = (Σl²+Σr²)/frames).
    const qint64 hops = (frames + envHop - 1) / envHop;
    QVector<double> hopSum(int(hops), 0.0);
    QVector<int> hopFrames(int(hops), 0);
    for (qint64 i = 0; i < frames; ++i) {
        double l = hL.process(sL.process(s[i * 2] / 32768.0));
        double r = hR.process(sR.process(s[i * 2 + 1] / 32768.0));
        const int h = int(i / envHop);
        hopSum[h] += l * l + r * r;
        hopFrames[h] += 1;
    }

    // Loudness a corto plazo: ventana deslizante de 400 ms (en hops).
    const int win = qMax(1, int(qint64(0.4 * rate) / envHop));
    shortEnv.resize(int(hops));
    double wSum = 0.0; qint64 wFrames = 0;
    for (int h = 0; h < hops; ++h) {
        wSum += hopSum[h]; wFrames += hopFrames[h];
        if (h >= win) { wSum -= hopSum[h - win]; wFrames -= hopFrames[h - win]; }
        const double z = wFrames > 0 ? wSum / wFrames : 0.0;
        shortEnv[h] = float(z > 0.0 ? -0.691 + 10.0 * std::log10(z) : -70.0);
    }

    // Integrado: bloques no solapados de 400 ms con puerta absoluta a −70 LUFS.
    double sum = 0.0; int kept = 0;
    for (int b = 0; b + win <= hops; b += win) {
        double bs = 0.0; qint64 bf = 0;
        for (int h = b; h < b + win; ++h) { bs += hopSum[h]; bf += hopFrames[h]; }
        const double z = bf > 0 ? bs / bf : 0.0;
        if (z > 0.0 && -0.691 + 10.0 * std::log10(z) > -70.0) { sum += z; ++kept; }
    }
    if (kept == 0) return -70.0;
    return -0.691 + 10.0 * std::log10(sum / kept);
}

double AudioPlayer::envMax(int trackIndex) const
{
    if (trackIndex < 0 || trackIndex >= m_env.size()) return 0.0;
    double mx = 0.0;
    for (float v : m_env[trackIndex]) mx = qMax(mx, double(v));
    return mx;
}

double AudioPlayer::lufsShortMax() const
{
    double mx = -70.0;
    for (float v : m_lufsEnv) mx = qMax(mx, double(v));
    return mx;
}

void AudioPlayer::setMix(const QVector<AudioMixClip> &clips, qint64 endUs,
                         double masterGain, double masterPan)
{
    if (!qEnvironmentVariableIsEmpty("PVS_AUDIO_DEBUG"))
        qInfo("[AUDIO] setMix recibido: %lld clips, fin=%.1fs", qint64(clips.size()), endUs / 1e6);
    const bool wasPlaying = m_pump && m_pump->isActive();
    const qint64 keepCursor = m_cursor;
    bake(clips, endUs, masterGain, masterPan);
    if (wasPlaying) m_cursor = qBound<qint64>(0, keepCursor, m_master.size());
    emit mixReady(m_lufs);
}

void AudioPlayer::startAt(qint64 startMs)
{
    stop();
    if (m_master.isEmpty()) return;
    qint64 off = (qint64((startMs / 1000.0) * kOutRate)) * 4;
    off &= ~qint64(3);
    m_cursor = qBound<qint64>(0, off, m_master.size());

    QAudioFormat fmt;
    fmt.setSampleRate(kOutRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!dev.isNull() && dev.isFormatSupported(fmt)) {
        m_sink = new QAudioSink(dev, fmt, this);
        m_dev = m_sink->start();
    }

    if (!m_pump) {
        m_pump = new QTimer(this);
        m_pump->setInterval(15);
        connect(m_pump, &QTimer::timeout, this, &AudioPlayer::pump);
    }
    m_pump->start();
}

void AudioPlayer::pump()
{
    if (m_cursor >= m_master.size()) {
        stop();
        emit finished();
        return;
    }
    double mL = 0.0, mR = 0.0;
    auto accum = [&](qint64 from, qint64 bytes) {
        const auto *s = reinterpret_cast<const int16_t *>(m_master.constData() + from);
        const int cnt = int(bytes / 2);
        for (int i = 0; i < cnt; ++i) {
            const double a = std::fabs(s[i] / 32768.0);
            if (i & 1) { if (a > mR) mR = a; }
            else       { if (a > mL) mL = a; }
        }
    };

    if (m_dev) {
        int budget = m_sink->bytesFree();
        while (budget > 0 && m_cursor < m_master.size()) {
            int n = qMin<qint64>(budget, m_master.size() - m_cursor) & ~3;
            if (n <= 0) break;
            m_dev->write(m_master.constData() + m_cursor, n);
            accum(m_cursor, n);
            m_cursor += n;
            budget -= n;
        }
    } else {
        // Sin dispositivo (headless): avanza ~15 ms para progresar medidores y el fin.
        qint64 n = (qint64((15 / 1000.0) * kOutRate) * 4) & ~qint64(3);
        n = qMin<qint64>(n, m_master.size() - m_cursor);
        if (n > 0) { accum(m_cursor, n); m_cursor += n; }
    }

    QVariantList peaks;
    const qint64 ei = (m_cursor / 4) / kEnvHop;
    for (const auto &e : m_env)
        peaks.push_back(ei < e.size() ? double(e[int(ei)]) : 0.0);
    const double shortLufs = ei < m_lufsEnv.size() ? double(m_lufsEnv[int(ei)]) : -70.0;
    emit meters(peaks, mL, mR, shortLufs);
}

void AudioPlayer::stop()
{
    if (m_pump) m_pump->stop();
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_dev = nullptr;
    }
    QVariantList zeros;
    for (int i = 0; i < m_env.size(); ++i) zeros.push_back(0.0);
    emit meters(zeros, 0.0, 0.0, -70.0);
}

// ---------------------------------------------------------------------------
// AudioEngine (fachada, hilo GUI)
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<AudioMixClip>("AudioMixClip");
    qRegisterMetaType<QVector<AudioMixClip>>("QVector<AudioMixClip>");

    m_thread = new QThread(this);
    m_thread->setObjectName("AudioThread");
    m_player = new AudioPlayer;
    m_player->moveToThread(m_thread);

    connect(m_thread, &QThread::finished, m_player, &QObject::deleteLater);
    connect(this, &AudioEngine::requestMix, m_player, &AudioPlayer::setMix);
    connect(this, &AudioEngine::requestStart, m_player, &AudioPlayer::startAt);
    connect(this, &AudioEngine::requestStop, m_player, &AudioPlayer::stop);

    connect(m_player, &AudioPlayer::meters, this,
            [this](const QVariantList &tp, double l, double r, double shortLufs) {
                m_trackPeaks = tp;
                m_peakL = l;
                m_peakR = r;
                m_lufsShort = shortLufs;
                emit levelChanged();
            });
    connect(m_player, &AudioPlayer::mixReady, this, [this](double lufs) {
        m_lufs = lufs;
        emit lufsChanged();
    });
    connect(m_player, &AudioPlayer::finished, this, [this]() {
        if (m_playing) { m_playing = false; emit playingChanged(); }
    });

    // Debounce del rehorneado: coalescer ráfagas de ediciones (arrastrar un slider) en un
    // solo horneado tras un breve reposo.
    m_rebake = new QTimer(this);
    m_rebake->setSingleShot(true);
    m_rebake->setInterval(180);
    connect(m_rebake, &QTimer::timeout, this,
            [this]() { emit requestMix(m_pendingClips, m_pendingEnd,
                                       m_pendingMasterGain, m_pendingMasterPan); });

    m_thread->start();
}

AudioEngine::~AudioEngine()
{
    emit requestStop();
    m_thread->quit();
    m_thread->wait(2000);
}

void AudioEngine::setMixData(const QVector<AudioMixClip> &clips, qint64 endUs,
                            double masterGain, double masterPan)
{
    m_pendingClips = clips;
    m_pendingEnd = endUs;
    m_pendingMasterGain = masterGain;
    m_pendingMasterPan = masterPan;
    m_rebake->start(); // (re)arranca el temporizador; hornea cuando cesan las ediciones
}

double AudioEngine::trackPeak(int trackIndex) const
{
    if (trackIndex < 0 || trackIndex >= m_trackPeaks.size()) return 0.0;
    return m_trackPeaks.at(trackIndex).toDouble();
}

void AudioEngine::playFrom(qint64 ms)
{
    emit requestStart(ms);
    if (!m_playing) { m_playing = true; emit playingChanged(); }
}

void AudioEngine::stop()
{
    emit requestStop();
    if (m_playing) { m_playing = false; emit playingChanged(); }
}

// ---------------------------------------------------------------------------
// Auto-test determinista (PVS_AUDIO_SELFTEST)
// ---------------------------------------------------------------------------

namespace {

void putLE16(QByteArray &b, quint16 v) { b.append(char(v & 0xFF)); b.append(char(v >> 8)); }
void putLE32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF));
}

// WAV PCM S16 estéreo 48 kHz. amp=0 => silencio; si no, tono de 1 kHz.
bool writeToneWav(const QString &path, double amp, double seconds)
{
    const int rate = 48000, ch = 2;
    const int n = int(rate * seconds);
    QByteArray data;
    data.reserve(n * ch * 2);
    for (int i = 0; i < n; ++i) {
        const double s = amp * std::sin(2.0 * M_PI * 1000.0 * i / rate);
        const auto v = int16_t(std::clamp(int(std::lround(s * 32767.0)), -32768, 32767));
        putLE16(data, quint16(v));
        putLE16(data, quint16(v));
    }
    QByteArray wav;
    wav.append("RIFF"); putLE32(wav, quint32(36 + data.size())); wav.append("WAVE");
    wav.append("fmt "); putLE32(wav, 16); putLE16(wav, 1); putLE16(wav, ch);
    putLE32(wav, rate); putLE32(wav, quint32(rate * ch * 2)); putLE16(wav, quint16(ch * 2));
    putLE16(wav, 16); wav.append("data"); putLE32(wav, quint32(data.size())); wav.append(data);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(wav); f.close();
    return true;
}

double masterPeak(const QByteArray &m, int channel /*0=L,1=R,-1=ambos*/)
{
    const qint64 frames = m.size() / 4;
    const auto *s = reinterpret_cast<const int16_t *>(m.constData());
    double mx = 0.0;
    for (qint64 i = 0; i < frames; ++i) {
        if (channel != 1) mx = qMax(mx, std::fabs(s[i * 2] / 32768.0));
        if (channel != 0) mx = qMax(mx, std::fabs(s[i * 2 + 1] / 32768.0));
    }
    return mx;
}

// Pico de un canal en el rango de tiempo [fromMs, toMs).
double masterPeakRange(const QByteArray &m, int channel, double fromMs, double toMs)
{
    const qint64 frames = m.size() / 4;
    const auto *s = reinterpret_cast<const int16_t *>(m.constData());
    const qint64 a = qBound<qint64>(0, qint64(fromMs / 1000.0 * 48000), frames);
    const qint64 b = qBound<qint64>(0, qint64(toMs / 1000.0 * 48000), frames);
    double mx = 0.0;
    for (qint64 i = a; i < b; ++i) {
        if (channel != 1) mx = qMax(mx, std::fabs(s[i * 2] / 32768.0));
        if (channel != 0) mx = qMax(mx, std::fabs(s[i * 2 + 1] / 32768.0));
    }
    return mx;
}

} // namespace

int runAudioSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_AUDIO_SELFTEST"))
        return -1;

    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        qInfo("[AUDIO-SELFTEST] %-40s %s", name, ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    };

    const QString dir = QDir::tempPath();
    const QString tone = dir + "/pvs_tone.wav";
    const QString sil = dir + "/pvs_silence.wav";
    check(writeToneWav(tone, 0.5, 0.6), "genera WAV de tono");
    check(writeToneWav(sil, 0.0, 0.6), "genera WAV de silencio");

    auto clip = [&](const QString &p, int trk, qint64 start, qint64 dur, qint64 in,
                    double sp, double g, double pan) {
        AudioMixClip c;
        c.mediaPath = p; c.trackIndex = trk; c.startUs = start; c.durationUs = dur;
        c.inUs = in; c.speed = sp; c.gain = g; c.pan = pan;
        return c;
    };

    // 1) Dos clips solapados en pistas distintas (mismo tono, misma fase) → el master suma.
    {
        AudioPlayer p;
        QVector<AudioMixClip> mix{
            clip(tone, 3, 0, 500000, 0, 1.0, 1.0, 0.0),
            clip(tone, 4, 0, 500000, 0, 1.0, 1.0, 0.0),
        };
        p.setMix(mix, 500000);
        const double pk = masterPeak(p.master(), -1);
        check(pk > 0.85, "mezcla: dos tonos solapados suman (pico > 0.85)");
        check(p.envMax(3) > 0.4 && p.envMax(4) > 0.4, "mezcla: envolventes de A1 y A2 activas");
        check(p.envMax(5) < 0.02, "mezcla: pista sin clip (A3) queda en silencio");
        // Tono a fondo de escala → ~0 LUFS (con realce K-weighting puede ser ligeramente > 0).
        check(p.lufs() > -30.0 && p.lufs() < 6.0, "LUFS del tono en rango razonable");
    }

    // 2) Ganancia: un clip a gain 0.5 reduce el pico a la mitad (~0.25 sobre amp 0.5).
    {
        AudioPlayer p;
        p.setMix({ clip(tone, 3, 0, 400000, 0, 1.0, 0.5, 0.0) }, 400000);
        const double pk = masterPeak(p.master(), -1);
        check(pk > 0.18 && pk < 0.30, "ganancia 0.5 escala el pico (~0.25)");
    }

    // 3) Pan a la izquierda: canal derecho ≈ 0, izquierdo presente.
    {
        AudioPlayer p;
        p.setMix({ clip(tone, 3, 0, 400000, 0, 1.0, 1.0, -1.0) }, 400000);
        check(masterPeak(p.master(), 0) > 0.4, "pan izq: canal L presente");
        check(masterPeak(p.master(), 1) < 0.05, "pan izq: canal R ≈ 0");
    }

    // 3b) Bus MAIN: ganancia del master 0.5 escala el pico a la mitad; pan del master
    //     izquierda vacía el canal derecho (sobre un clip centrado a ganancia 1).
    {
        AudioPlayer full, half;
        full.setMix({ clip(tone, 3, 0, 400000, 0, 1.0, 1.0, 0.0) }, 400000, 1.0, 0.0);
        half.setMix({ clip(tone, 3, 0, 400000, 0, 1.0, 1.0, 0.0) }, 400000, 0.5, 0.0);
        const double pf = masterPeak(full.master(), -1), ph = masterPeak(half.master(), -1);
        check(pf > 0.4 && std::fabs(ph - pf * 0.5) < 0.05, "master gain 0.5 escala el pico a la mitad");

        AudioPlayer pl;
        pl.setMix({ clip(tone, 3, 0, 400000, 0, 1.0, 1.0, 0.0) }, 400000, 1.0, -1.0);
        check(masterPeak(pl.master(), 0) > 0.4, "master pan izq: canal L presente");
        check(masterPeak(pl.master(), 1) < 0.05, "master pan izq: canal R ≈ 0");
    }

    // 4) Velocidad 2×: el clip de 400 ms de salida consume 800 ms de origen (no recorta pico).
    {
        AudioPlayer p;
        p.setMix({ clip(tone, 3, 0, 400000, 0, 2.0, 1.0, 0.0) }, 400000);
        check(masterPeak(p.master(), -1) > 0.4, "velocidad 2×: pico presente");
    }

    // 5) Silencio → LUFS de puerta (−70) y pico ~0.
    {
        AudioPlayer p;
        p.setMix({ clip(sil, 3, 0, 400000, 0, 1.0, 1.0, 0.0) }, 400000);
        check(masterPeak(p.master(), -1) < 0.02, "silencio: pico ≈ 0");
        check(p.lufs() <= -60.0, "silencio: LUFS ≈ puerta");
    }

    // 6) Mute del clip → el master queda en silencio.
    {
        AudioPlayer p;
        AudioMixClip c = clip(tone, 3, 0, 400000, 0, 1.0, 1.0, 0.0);
        c.mute = true;
        p.setMix({ c }, 400000);
        check(masterPeak(p.master(), -1) < 0.02, "mute del clip: master en silencio");
    }

    // 7) Automatización de pan: rampa de izquierda (−1) a derecha (+1) a lo largo del clip.
    {
        AudioPlayer p;
        AudioMixClip c = clip(tone, 3, 0, 500000, 0, 1.0, 1.0, 0.0);
        c.panKf = { { 0, -1.0 }, { 500000, 1.0 } }; // sourceUs → pan
        p.setMix({ c }, 500000);
        const double lStart = masterPeakRange(p.master(), 0, 0, 50);
        const double rStart = masterPeakRange(p.master(), 1, 0, 50);
        const double lEnd = masterPeakRange(p.master(), 0, 450, 500);
        const double rEnd = masterPeakRange(p.master(), 1, 450, 500);
        // La rampa mueve el balance: L decrece y R crece a lo largo del clip.
        check(lStart > 0.4 && lStart > lEnd + 0.3, "pan automatizado: L decrece (izq→der)");
        check(rEnd > 0.4 && rEnd > rStart + 0.3, "pan automatizado: R crece (izq→der)");
    }

    // 8) Envolvente de loudness a corto plazo poblada para un tono.
    {
        AudioPlayer p;
        p.setMix({ clip(tone, 3, 0, 600000, 0, 1.0, 1.0, 0.0) }, 600000);
        check(p.lufsShortMax() > -30.0, "LUFS a corto plazo: envolvente poblada");
    }

    // Pico del master en un rango de tiempo (para medir en régimen permanente,
    // saltándose los transitorios de ataque de los filtros/compresor).
    auto peakRange = [](const QByteArray &m, double t0, double t1) {
        const auto *s = reinterpret_cast<const int16_t *>(m.constData());
        const qint64 frames = m.size() / 4;
        qint64 a = qBound<qint64>(0, qint64(t0 * 48000), frames);
        qint64 b = qBound<qint64>(a, qint64(t1 * 48000), frames);
        double pk = 0.0;
        for (qint64 i = a; i < b; ++i)
            pk = qMax(pk, qMax(std::fabs(s[i * 2] / 32768.0), std::fabs(s[i * 2 + 1] / 32768.0)));
        return pk;
    };

    // 9) EQ de pista: sobre el tono de 1 kHz, realzar/atenuar la banda MEDIA (peak @1 kHz)
    //    sube/baja el pico. Con todas las bandas a 0 dB el EQ es transparente.
    {
        auto eqClip = [&](double midDb) {
            AudioMixClip c = clip(tone, 3, 0, 400000, 0, 1.0, 0.4, 0.0);   // 0.4: margen para el boost
            c.eqOn = true; c.eqMidDb = midDb;
            return c;
        };
        AudioPlayer flat, boost, cut;
        flat.setMix({ eqClip(0.0) }, 400000);
        boost.setMix({ eqClip(12.0) }, 400000);
        cut.setMix({ eqClip(-12.0) }, 400000);
        const double pf = masterPeak(flat.master(), -1);
        const double pb = masterPeak(boost.master(), -1);
        const double pc = masterPeak(cut.master(), -1);
        check(pb > pf * 1.5, "EQ: realce de medios sube el pico del tono de 1 kHz");
        check(pc < pf * 0.7, "EQ: atenuacion de medios baja el pico del tono de 1 kHz");
    }

    // 10) Compresor de pista: reduce el pico de una señal por encima del umbral.
    //     Medido en régimen permanente (0.2–0.4 s) para evitar el ataque de 10 ms.
    {
        auto compClip = [&](bool on) {
            AudioMixClip c = clip(tone, 3, 0, 400000, 0, 1.0, 1.0, 0.0);   // pico ~0.5 (−6 dB)
            c.compOn = on; c.compThreshDb = -18.0; c.compRatio = 4.0; c.compMakeupDb = 0.0;
            return c;
        };
        AudioPlayer off, on;
        off.setMix({ compClip(false) }, 400000);
        on.setMix({ compClip(true) }, 400000);
        const double poff = peakRange(off.master(), 0.2, 0.4);
        const double pon = peakRange(on.master(), 0.2, 0.4);
        check(poff > 0.4, "compresor: señal de referencia por encima del umbral");
        check(pon < poff * 0.75, "compresor reduce el pico por encima del umbral");
    }

    QFile::remove(tone);
    QFile::remove(sil);
    qInfo("[AUDIO-SELFTEST] %d fallo(s)", failures);
    return failures == 0 ? 0 : 1;
}
