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
#include <memory>
#include <new>
#include <vector>

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
    void setCoeffs(const Biquad &s) { b0 = s.b0; b1 = s.b1; b2 = s.b2; a1 = s.a1; a2 = s.a2; }  // conserva z1/z2
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

// Procesadores de audio con ESTADO PERSISTENTE: procesan un buffer estéreo intercalado
// (`ptr`, `n` frames) manteniendo su estado entre llamadas, para hornear por bloques
// sin discontinuidades. Un solo `set()` fija los coeficientes; luego se llama a
// `process()` con cada bloque contiguo.

// EQ de 3 bandas (low-shelf 120 Hz · peak 1 kHz · high-shelf 8 kHz).
struct Eq3 {
    Biquad loL, loR, miL, miR, hiL, hiR;
    void set(double fs, double low, double mid, double high) {
        loL = makeLowShelf(fs, 120.0, low);    loR = loL;
        miL = makePeak(fs, 1000.0, 0.9, mid);   miR = miL;
        hiL = makeHighShelf(fs, 8000.0, high);  hiR = hiL;
    }
    // Actualiza los coeficientes (automatización) conservando el estado de los filtros.
    void retune(double fs, double low, double mid, double high) {
        const Biquad l = makeLowShelf(fs, 120.0, low), m = makePeak(fs, 1000.0, 0.9, mid),
                     h = makeHighShelf(fs, 8000.0, high);
        loL.setCoeffs(l); loR.setCoeffs(l);
        miL.setCoeffs(m); miR.setCoeffs(m);
        hiL.setCoeffs(h); hiR.setCoeffs(h);
    }
    void process(float *b, qint64 n) {
        for (qint64 f = 0; f < n; ++f) {
            b[f * 2]     = float(hiL.process(miL.process(loL.process(b[f * 2]))));
            b[f * 2 + 1] = float(hiR.process(miR.process(loR.process(b[f * 2 + 1]))));
        }
    }
};

// Compresor de pico (ataque 10 ms · relajación 100 ms).
struct Comp {
    double thresh = 1, makeup = 1, atk = 0, rel = 0, invR = 1, envc = 0.0;
    void set(double fs, double threshDb, double ratio, double makeupDb) {
        thresh = std::pow(10.0, threshDb / 20.0);
        makeup = std::pow(10.0, makeupDb / 20.0);
        atk = std::exp(-1.0 / (fs * 0.010));
        rel = std::exp(-1.0 / (fs * 0.100));
        invR = 1.0 / ratio;
    }
    void retune(double threshDb, double ratio, double makeupDb) {   // conserva envc
        thresh = std::pow(10.0, threshDb / 20.0);
        makeup = std::pow(10.0, makeupDb / 20.0);
        invR = 1.0 / ratio;
    }
    void process(float *b, qint64 n) {
        for (qint64 f = 0; f < n; ++f) {
            const double l = b[f * 2], r = b[f * 2 + 1];
            const double sidet = qMax(std::fabs(l), std::fabs(r));
            const double coef = sidet > envc ? atk : rel;
            envc = coef * envc + (1.0 - coef) * sidet;
            double gr = 1.0;
            if (envc > thresh && envc > 1e-9)
                gr = std::pow(envc / thresh, invR - 1.0);
            const double g = gr * makeup;
            b[f * 2] = float(l * g); b[f * 2 + 1] = float(r * g);
        }
    }
};

// Paso-bajo RBJ (Q = 0.707), para separar la banda de sibilancia del de-esser.
Biquad makeLowPass(double fs, double f0)
{
    const double w0 = 2.0 * kPi * f0 / fs, cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * 0.70710678);
    const double b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2;
    const double a0 = 1 + alpha, a1 = -2 * cs, a2 = 1 - alpha;
    return Biquad{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

// Puerta de ruido: por debajo del umbral, cierra (silencia). Abre 5 ms, cierra 100 ms.
// Empieza cerrada.
struct Gate {
    double thresh = 0, gAtk = 0, gRel = 0, dRel = 0, env = 0.0, g = 0.0;
    void set(double fs, double threshDb) {
        thresh = std::pow(10.0, threshDb / 20.0);
        gAtk = std::exp(-1.0 / (fs * 0.005));
        gRel = std::exp(-1.0 / (fs * 0.100));
        dRel = std::exp(-1.0 / (fs * 0.050));
    }
    void retune(double threshDb) { thresh = std::pow(10.0, threshDb / 20.0); }   // conserva env/g
    void process(float *b, qint64 n) {
        for (qint64 f = 0; f < n; ++f) {
            const double l = b[f * 2], r = b[f * 2 + 1];
            const double peak = qMax(std::fabs(l), std::fabs(r));
            env = peak > env ? peak : dRel * env + (1.0 - dRel) * peak;
            const double target = env >= thresh ? 1.0 : 0.0;
            const double coef = target > g ? gAtk : gRel;
            g = coef * g + (1.0 - coef) * target;
            b[f * 2] = float(l * g); b[f * 2 + 1] = float(r * g);
        }
    }
};

// De-esser: comprime SOLO la banda alta (> 6 kHz), separada con un paso-bajo. Ratio 4:1.
struct DeEss {
    Biquad lpL, lpR;
    double thresh = 0, atk = 0, rel = 0, invR = 0.25, env = 0.0;
    void set(double fs, double threshDb) {
        lpL = makeLowPass(fs, 6000.0); lpR = lpL;
        thresh = std::pow(10.0, threshDb / 20.0);
        atk = std::exp(-1.0 / (fs * 0.002));
        rel = std::exp(-1.0 / (fs * 0.050));
    }
    void retune(double threshDb) { thresh = std::pow(10.0, threshDb / 20.0); }   // conserva env/filtros
    void process(float *b, qint64 n) {
        for (qint64 f = 0; f < n; ++f) {
            const double l = b[f * 2], r = b[f * 2 + 1];
            const double lowL = lpL.process(l), lowR = lpR.process(r);
            const double hi0 = l - lowL, hi1 = r - lowR;
            const double det = qMax(std::fabs(hi0), std::fabs(hi1));
            const double coef = det > env ? atk : rel;
            env = coef * env + (1.0 - coef) * det;
            double gr = 1.0;
            if (env > thresh && env > 1e-9)
                gr = std::pow(env / thresh, invR - 1.0);
            b[f * 2] = float(lowL + hi0 * gr); b[f * 2 + 1] = float(lowR + hi1 * gr);
        }
    }
};

// Reverb algorítmica (Freeverb reducido): 4 combs + 2 all-pass sobre la mezcla mono.
struct Reverb {
    struct Comb { QVector<float> z; int i = 0; float fb = 0;
        void set(int d, float f) { z.fill(0.0f, d); i = 0; fb = f; }
        float p(float x) { float y = z[i]; z[i] = x + y * fb; if (++i >= z.size()) i = 0; return y; } };
    struct AP { QVector<float> z; int i = 0; float fb = 0;
        void set(int d, float f) { z.fill(0.0f, d); i = 0; fb = f; }
        float p(float x) { float y = z[i]; float o = y - x; z[i] = x + y * fb; if (++i >= z.size()) i = 0; return o; } };
    Comb c1, c2, c3, c4; AP a1, a2; float wet = 0, dry = 1;
    void set(double fs, double mix, double size) {
        const double sc = fs / 44100.0;
        const float fb = float(0.28 + qBound(0.0, size, 1.0) * 0.7);
        auto D = [sc](int d) { return qMax(1, int(d * sc)); };
        c1.set(D(1116), fb); c2.set(D(1188), fb); c3.set(D(1277), fb); c4.set(D(1356), fb);
        a1.set(D(556), 0.5f); a2.set(D(441), 0.5f);
        wet = float(qBound(0.0, mix, 1.0)); dry = 1.0f - wet;
    }
    void setMix(double mix) { wet = float(qBound(0.0, mix, 1.0)); dry = 1.0f - wet; }  // sin tocar las líneas de retardo
    void process(float *b, qint64 n) {
        for (qint64 f = 0; f < n; ++f) {
            const float l = b[f * 2], r = b[f * 2 + 1];
            const float in = (l + r) * 0.5f;
            float w = c1.p(in) + c2.p(in) + c3.p(in) + c4.p(in);
            w = a2.p(a1.p(w)) * 0.25f;
            b[f * 2] = l * dry + w * wet; b[f * 2 + 1] = r * dry + w * wet;
        }
    }
};

// Cadena de efectos (puerta → EQ → compresor → de-esser → reverb) con estado.
struct FxChain {
    bool gateOn = false, eqOn = false, compOn = false, deEssOn = false, reverbOn = false;
    Gate gate; Eq3 eq; Comp comp; DeEss de; Reverb rev;
    bool any() const { return gateOn || eqOn || compOn || deEssOn || reverbOn; }
    void process(float *b, qint64 n) {
        if (gateOn) gate.process(b, n);
        if (eqOn) eq.process(b, n);
        if (compOn) comp.process(b, n);
        if (deEssOn) de.process(b, n);
        if (reverbOn) rev.process(b, n);
    }
};

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
namespace {
// Lector de UN clip de audio en STREAMING: decodifica el origen por trozos y produce
// muestras de salida (48 kHz estéreo) con remapeo de velocidad (interpolación lineal),
// manteniendo su posición entre bloques. Evita decodificar el clip entero en RAM.
struct Voice {
    AudioPlayer dec;             // decodificador propio (constructor trivial)
    double speed = 1.0;
    QVector<float> src;         // origen bufferizado (float estéreo)
    qint64 srcBase = 0;         // frame de origen de src[0]
    bool eof = false, ok = false;
    void open(const AudioMixClip &c) {
        speed = c.speed > 0 ? c.speed : 1.0;
        ok = dec.openSource(c.mediaPath);
        if (ok) dec.seekMs(c.inUs / 1000);
    }
    void close() { dec.closeSource(); }
    void ensureUpto(qint64 uptoFrame) {
        while (!eof && srcBase + src.size() / 2 <= uptoFrame) {
            double pL, pR;
            const QByteArray chunk = dec.decodeChunk(pL, pR);
            if (chunk.isEmpty()) { eof = true; break; }
            const int n = int(chunk.size() / sizeof(int16_t));
            const auto *s = reinterpret_cast<const int16_t *>(chunk.constData());
            const int old = int(src.size());
            src.resize(old + n);
            for (int i = 0; i < n; ++i) src[old + i] = s[i] / 32768.0f;
        }
    }
    void trim(qint64 belowFrame) {
        const qint64 drop = belowFrame - srcBase;
        if (drop <= 0) return;
        src.remove(0, int(qMin<qint64>(drop * 2, src.size())));
        srcBase = belowFrame;
    }
    // Produce n frames de salida desde el frame de salida k0 (contiguo) en dst (limpio).
    void produce(float *dst, qint64 k0, qint64 n) {
        const qint64 firstSrc = qint64(std::floor(k0 * speed));
        trim(firstSrc);
        ensureUpto(qint64(std::floor((k0 + n - 1) * speed)) + 1);
        const qint64 srcFrames = srcBase + src.size() / 2;
        for (qint64 j = 0; j < n; ++j) {
            const double pos = (k0 + j) * speed;
            const qint64 i0 = qint64(std::floor(pos));
            const double f = pos - double(i0);
            for (int ch = 0; ch < 2; ++ch) {
                const float a = (i0 >= srcBase && i0 < srcFrames) ? src[int((i0 - srcBase) * 2 + ch)] : 0.0f;
                const float b = (i0 + 1 >= srcBase && i0 + 1 < srcFrames) ? src[int((i0 + 1 - srcBase) * 2 + ch)] : a;
                dst[j * 2 + ch] = a + (b - a) * float(f);
            }
        }
    }
};
} // namespace

void AudioPlayer::bake(const QVector<AudioMixClip> &clips, qint64 endUs,
                       double masterGain, double masterPan, bool limiterOn, double ceilingDb)
{
    m_master.clear();
    m_env.clear();
    m_lufs = -70.0;
    m_cursor = 0;

    if (endUs <= 0)
        for (const auto &c : clips) endUs = qMax(endUs, c.startUs + c.durationUs);
    const qint64 totalSamples = qint64((endUs / 1e6) * kOutRate);
    if (totalSamples <= 0) return;
    const double fs = double(kOutRate);

    // Horneado por BLOQUES: el único buffer grande es la salida S16 (~4 B/frame). Los
    // buffers float son de un bloque (1 s), no de toda la secuencia. Protegido.
    const qint64 masterBytes = totalSamples * 4;   // 2 canales × S16
    const double masterMB = masterBytes / (1024.0 * 1024.0);
    if (totalSamples > 30LL * 60 * kOutRate)
        qWarning("[audio] bake por bloques: %.1f s (%.1f min), master ~%.0f MB",
                 endUs / 1e6, endUs / 60e6, masterMB);
    if (masterBytes > 8LL * 1024 * 1024 * 1024) {   // tope de cordura: ~8 GB
        qCritical("[audio] bake abortado: %.0f min (master ~%.0f MB) supera el límite.",
                  endUs / 60e6, masterMB);
        return;
    }
    try {
        m_master.resize(masterBytes);
    } catch (const std::bad_alloc &) {
        qCritical("[audio] SIN MEMORIA para el master (~%.0f MB). Bake abortado.", masterMB);
        m_master.clear(); return;
    }

    int nTracks = 6; // A1–A3 viven en los índices 3–5
    for (const auto &c : clips) nTracks = qMax(nTracks, c.trackIndex + 1);
    const qint64 envLen = (totalSamples + kEnvHop - 1) / kEnvHop;
    m_env.resize(nTracks);
    for (auto &e : m_env) e.fill(0.0f, int(envLen));

    auto balance = [](double pan, double &gL, double &gR) {
        gL = pan <= 0.0 ? 1.0 : 1.0 - pan;
        gR = pan >= 0.0 ? 1.0 : 1.0 + pan;
    };
    auto cfgClipFx = [fs](FxChain &fx, const AudioMixClip &c) {
        fx.gateOn = c.clipGateOn;   if (fx.gateOn) fx.gate.set(fs, c.clipGateThreshDb);
        fx.eqOn = c.clipEqOn;       if (fx.eqOn) fx.eq.set(fs, c.clipEqLowDb, c.clipEqMidDb, c.clipEqHighDb);
        fx.compOn = c.clipCompOn;   if (fx.compOn) fx.comp.set(fs, c.clipCompThreshDb, c.clipCompRatio, c.clipCompMakeupDb);
        fx.deEssOn = c.clipDeEssOn; if (fx.deEssOn) fx.de.set(fs, c.clipDeEssThreshDb);
        fx.reverbOn = c.clipReverbOn; if (fx.reverbOn) fx.rev.set(fs, c.clipReverbMix, c.clipReverbSize);
    };
    auto cfgTrackFx = [fs](FxChain &fx, const AudioMixClip &c) {
        fx.gateOn = c.gateOn;   if (fx.gateOn) fx.gate.set(fs, c.gateThreshDb);
        fx.eqOn = c.eqOn;       if (fx.eqOn) fx.eq.set(fs, c.eqLowDb, c.eqMidDb, c.eqHighDb);
        fx.compOn = c.compOn;   if (fx.compOn) fx.comp.set(fs, c.compThreshDb, c.compRatio, c.compMakeupDb);
        fx.deEssOn = c.deEssOn; if (fx.deEssOn) fx.de.set(fs, c.deEssThreshDb);
        fx.reverbOn = c.reverbOn; if (fx.reverbOn) fx.rev.set(fs, c.reverbMix, c.reverbSize);
    };

    // Estado por clip (voz de streaming + efectos por clip) y por pista (efectos).
    struct ClipVoice { const AudioMixClip *c; Voice v; FxChain fx; qint64 startSample, outSamples; };
    struct TrackBake { int trackIndex; QVector<int> clipIdx; FxChain fx; };

    QHash<int, QVector<int>> byTrack;
    for (int i = 0; i < clips.size(); ++i)
        if (!clips[i].mute && !clips[i].mediaPath.isEmpty())
            byTrack[clips[i].trackIndex].push_back(i);

    std::vector<std::unique_ptr<ClipVoice>> voice(clips.size());
    std::vector<TrackBake> tracks;
    for (auto it = byTrack.constBegin(); it != byTrack.constEnd(); ++it) {
        TrackBake tb; tb.trackIndex = it.key(); tb.clipIdx = it.value();
        cfgTrackFx(tb.fx, clips[tb.clipIdx.first()]);   // fx de pista (iguales para todos)
        for (int ci : tb.clipIdx) {
            auto cv = std::make_unique<ClipVoice>();
            cv->c = &clips[ci];
            cv->startSample = qint64((clips[ci].startUs / 1e6) * kOutRate);
            cv->outSamples = qint64((clips[ci].durationUs / 1e6) * kOutRate);
            cfgClipFx(cv->fx, clips[ci]);
            cv->v.open(clips[ci]);
            voice[ci] = std::move(cv);
        }
        tracks.push_back(std::move(tb));
    }

    // Bucle principal por bloques de 1 s.
    const qint64 CHUNK = kOutRate;
    QVector<float> trackBuf(int(CHUNK * 2)), masterBuf(int(CHUNK * 2)), tmp(int(CHUNK * 2));

    double mgL, mgR; balance(masterPan, mgL, mgR);
    const double mL = masterGain * mgL, mR = masterGain * mgR;
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    const double relL = std::exp(-1.0 / (fs * 0.050));
    double lgr = 1.0;
    auto *out = reinterpret_cast<int16_t *>(m_master.data());

    for (qint64 chunkStart = 0; chunkStart < totalSamples; chunkStart += CHUNK) {
        const qint64 chunkLen = qMin<qint64>(CHUNK, totalSamples - chunkStart);
        std::fill_n(masterBuf.data(), chunkLen * 2, 0.0f);

        for (TrackBake &tb : tracks) {
            std::fill_n(trackBuf.data(), chunkLen * 2, 0.0f);
            bool active = false;
            for (int ci : tb.clipIdx) {
                ClipVoice &cv = *voice[ci];
                const qint64 lo = qMax(chunkStart, cv.startSample);
                const qint64 hi = qMin(chunkStart + chunkLen, cv.startSample + cv.outSamples);
                if (lo >= hi) continue;
                active = true;
                const qint64 k0 = lo - cv.startSample, nn = hi - lo, off = lo - chunkStart;
                const AudioMixClip &c = *cv.c;
                // Automatización por keyframes de los efectos: reevalúa por bloque en el
                // tiempo de origen del clip y re-sintoniza conservando el estado.
                const qint64 srcUs = c.inUs + qint64((k0 / double(kOutRate)) * 1e6 * c.speed);
                if (cv.fx.eqOn && (!c.clipEqLowKf.isEmpty() || !c.clipEqMidKf.isEmpty() || !c.clipEqHighKf.isEmpty()))
                    cv.fx.eq.retune(fs, evalGainKf(c.clipEqLowKf, c.clipEqLowDb, srcUs),
                                    evalGainKf(c.clipEqMidKf, c.clipEqMidDb, srcUs),
                                    evalGainKf(c.clipEqHighKf, c.clipEqHighDb, srcUs));
                if (cv.fx.reverbOn && !c.clipReverbMixKf.isEmpty())
                    cv.fx.rev.setMix(evalGainKf(c.clipReverbMixKf, c.clipReverbMix, srcUs));
                if (cv.fx.compOn && (!c.clipCompThreshKf.isEmpty() || !c.clipCompRatioKf.isEmpty() || !c.clipCompMakeupKf.isEmpty()))
                    cv.fx.comp.retune(evalGainKf(c.clipCompThreshKf, c.clipCompThreshDb, srcUs),
                                      evalGainKf(c.clipCompRatioKf, c.clipCompRatio, srcUs),
                                      evalGainKf(c.clipCompMakeupKf, c.clipCompMakeupDb, srcUs));
                if (cv.fx.gateOn && !c.clipGateThreshKf.isEmpty())
                    cv.fx.gate.retune(evalGainKf(c.clipGateThreshKf, c.clipGateThreshDb, srcUs));
                if (cv.fx.deEssOn && !c.clipDeEssThreshKf.isEmpty())
                    cv.fx.de.retune(evalGainKf(c.clipDeEssThreshKf, c.clipDeEssThreshDb, srcUs));
                cv.v.produce(tmp.data(), k0, nn);   // trozo limpio del clip
                cv.fx.process(tmp.data(), nn);      // efectos por clip (estado continuo)
                double sgL, sgR; balance(c.pan, sgL, sgR);
                const bool needSrc = !c.gainKf.isEmpty() || !c.panKf.isEmpty();
                for (qint64 j = 0; j < nn; ++j) {
                    double g = c.gain, gL = sgL, gR = sgR;
                    if (needSrc) {
                        const qint64 srcUs = c.inUs + qint64(((k0 + j) / double(kOutRate)) * 1e6 * c.speed);
                        if (!c.gainKf.isEmpty()) g = evalGainKf(c.gainKf, c.gain, srcUs);
                        if (!c.panKf.isEmpty()) balance(evalGainKf(c.panKf, c.pan, srcUs), gL, gR);
                    }
                    trackBuf[int((off + j) * 2)]     += tmp[int(j * 2)]     * float(g * gL);
                    trackBuf[int((off + j) * 2 + 1)] += tmp[int(j * 2 + 1)] * float(g * gR);
                }
            }
            // Efectos de pista. La reverb procesa siempre (su cola suena en el silencio);
            // el resto solo cuando la pista tiene clip activo en el bloque.
            if (tb.fx.any() && (active || tb.fx.reverbOn))
                tb.fx.process(trackBuf.data(), chunkLen);

            QVector<float> &env = m_env[qBound(0, tb.trackIndex, nTracks - 1)];
            for (qint64 j = 0; j < chunkLen; ++j) {
                const float l = trackBuf[int(j * 2)], r = trackBuf[int(j * 2 + 1)];
                masterBuf[int(j * 2)]     += l;
                masterBuf[int(j * 2 + 1)] += r;
                const float a = qMax(std::fabs(l), std::fabs(r));
                float &ev = env[int((chunkStart + j) / kEnvHop)];
                if (a > ev) ev = a;
            }
        }

        // Bus MAIN: ganancia + pan + limitador (estado continuo) → S16.
        for (qint64 j = 0; j < chunkLen; ++j) {
            double l = masterBuf[int(j * 2)] * mL, r = masterBuf[int(j * 2 + 1)] * mR;
            if (limiterOn) {
                const double peak = qMax(std::fabs(l), std::fabs(r));
                const double target = (peak > ceiling && peak > 1e-9) ? ceiling / peak : 1.0;
                if (target < lgr) lgr = target;
                else lgr = relL * lgr + (1.0 - relL) * target;
                l *= lgr; r *= lgr;
            }
            const qint64 idx = chunkStart + j;
            out[idx * 2]     = int16_t(std::clamp(int(std::lround(l * 32767.0)), -32768, 32767));
            out[idx * 2 + 1] = int16_t(std::clamp(int(std::lround(r * 32767.0)), -32768, 32767));
        }
    }

    for (auto &pv : voice) if (pv) pv->v.close();   // libera decodificadores/ficheros

    m_lufs = kWeightAnalyze(m_master, kOutRate, kEnvHop, m_lufsEnv);

    if (!qEnvironmentVariableIsEmpty("PVS_AUDIO_DEBUG"))
        qInfo("[AUDIO] horneado por bloques: %lld clips, fin=%.1fs, master=%lld frames, LUFS=%.1f",
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
                         double masterGain, double masterPan, bool limiterOn, double ceilingDb)
{
    if (!qEnvironmentVariableIsEmpty("PVS_AUDIO_DEBUG"))
        qInfo("[AUDIO] setMix recibido: %lld clips, fin=%.1fs", qint64(clips.size()), endUs / 1e6);
    const bool wasPlaying = m_pump && m_pump->isActive();
    const qint64 keepCursor = m_cursor;
    bake(clips, endUs, masterGain, masterPan, limiterOn, ceilingDb);
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
                                       m_pendingMasterGain, m_pendingMasterPan,
                                       m_pendingLimiterOn, m_pendingCeilingDb); });

    m_thread->start();
}

AudioEngine::~AudioEngine()
{
    emit requestStop();
    m_thread->quit();
    m_thread->wait(2000);
}

void AudioEngine::setMixData(const QVector<AudioMixClip> &clips, qint64 endUs,
                            double masterGain, double masterPan, bool limiterOn, double ceilingDb)
{
    m_pendingClips = clips;
    m_pendingEnd = endUs;
    m_pendingMasterGain = masterGain;
    m_pendingMasterPan = masterPan;
    m_pendingLimiterOn = limiterOn;
    m_pendingCeilingDb = ceilingDb;
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
bool writeToneWav(const QString &path, double amp, double seconds, double freq = 1000.0)
{
    const int rate = 48000, ch = 2;
    const int n = int(rate * seconds);
    QByteArray data;
    data.reserve(n * ch * 2);
    for (int i = 0; i < n; ++i) {
        const double s = amp * std::sin(2.0 * M_PI * freq * i / rate);
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

    // 11) Limitador del master: el pico nunca supera el techo (techo −6 dBFS ≈ 0.501).
    {
        const AudioMixClip c = clip(tone, 3, 0, 400000, 0, 1.0, 1.0, 0.0);
        AudioPlayer off, on;
        off.setMix({ c, c }, 400000, 1.0, 0.0, false, -6.0);   // dos clips → suma satura
        on.setMix({ c, c }, 400000, 1.0, 0.0, true, -6.0);
        const double ceiling = std::pow(10.0, -6.0 / 20.0);
        check(masterPeak(off.master(), -1) > ceiling, "sin limitador el pico supera el techo");
        check(masterPeak(on.master(), -1) <= ceiling * 1.02, "el limitador mantiene el pico bajo el techo");
    }

    // 12) EQ POR CLIP: realza/atenúa la banda media del tono de 1 kHz (independiente del de pista).
    {
        auto eqClip = [&](double midDb) {
            AudioMixClip c = clip(tone, 3, 0, 400000, 0, 1.0, 0.4, 0.0);
            c.clipEqOn = true; c.clipEqMidDb = midDb;
            return c;
        };
        AudioPlayer flat, boost, cut;
        flat.setMix({ eqClip(0.0) }, 400000);
        boost.setMix({ eqClip(12.0) }, 400000);
        cut.setMix({ eqClip(-12.0) }, 400000);
        const double pf = masterPeak(flat.master(), -1);
        check(masterPeak(boost.master(), -1) > pf * 1.5, "EQ por clip: realce de medios sube el pico");
        check(masterPeak(cut.master(), -1) < pf * 0.7, "EQ por clip: atenuacion de medios baja el pico");
    }

    // 13) Puerta de ruido: una señal por debajo del umbral queda silenciada.
    {
        const QString quiet = dir + "/pvs_quiet.wav";
        writeToneWav(quiet, 0.03, 0.4);   // ≈ −30 dBFS
        auto gclip = [&](bool gate) {
            AudioMixClip c = clip(quiet, 3, 0, 400000, 0, 1.0, 1.0, 0.0);
            c.gateOn = gate; c.gateThreshDb = -20.0;   // umbral −20 dBFS
            return c;
        };
        AudioPlayer off, on;
        off.setMix({ gclip(false) }, 400000);
        on.setMix({ gclip(true) }, 400000);
        check(masterPeak(off.master(), -1) > 0.02, "puerta: señal floja presente sin puerta");
        check(peakRange(on.master(), 0.15, 0.4) < 0.005, "puerta cierra la señal bajo el umbral");
        QFile::remove(quiet);
    }

    // 14) De-esser: atenúa un tono AGUDO (7 kHz, banda de sibilancia); no toca uno grave.
    {
        const QString hi = dir + "/pvs_hi.wav";
        writeToneWav(hi, 0.5, 0.4, 7000.0);
        auto dclip = [&](const QString &p, bool de) {
            AudioMixClip c = clip(p, 3, 0, 400000, 0, 1.0, 1.0, 0.0);
            c.deEssOn = de; c.deEssThreshDb = -30.0;
            return c;
        };
        AudioPlayer offHi, onHi, onLo;
        offHi.setMix({ dclip(hi, false) }, 400000);
        onHi.setMix({ dclip(hi, true) }, 400000);
        onLo.setMix({ dclip(tone, true) }, 400000);   // 1 kHz con de-esser
        const double pOffHi = peakRange(offHi.master(), 0.1, 0.4);
        check(peakRange(onHi.master(), 0.1, 0.4) < pOffHi * 0.85, "de-esser atenúa el tono agudo (7 kHz)");
        check(peakRange(onLo.master(), 0.1, 0.4) > 0.4, "de-esser no toca el tono grave (1 kHz)");
        QFile::remove(hi);
    }

    // 15) Reverb: añade cola después de una ráfaga corta.
    {
        auto rclip = [&](bool rev) {
            AudioMixClip c = clip(tone, 3, 0, 80000, 0, 1.0, 1.0, 0.0);   // ráfaga de 80 ms
            c.reverbOn = rev; c.reverbMix = 0.8; c.reverbSize = 0.85;
            return c;
        };
        AudioPlayer off, on;
        off.setMix({ rclip(false) }, 500000);
        on.setMix({ rclip(true) }, 500000);
        check(peakRange(off.master(), 0.2, 0.45) < 0.005, "sin reverb no hay cola tras la ráfaga");
        check(peakRange(on.master(), 0.2, 0.45) > 0.01, "reverb añade cola después de la ráfaga");
    }

    // 16) Procesadores POR CLIP: la puerta por clip silencia por debajo del umbral
    //     (confirma el enrutado clip → PCM; la DSP la validan los tests de pista).
    {
        const QString quiet = dir + "/pvs_quiet2.wav";
        writeToneWav(quiet, 0.03, 0.4);
        auto gc = [&](bool gate) {
            AudioMixClip c = clip(quiet, 3, 0, 400000, 0, 1.0, 1.0, 0.0);
            c.clipGateOn = gate; c.clipGateThreshDb = -20.0;
            return c;
        };
        AudioPlayer off, on;
        off.setMix({ gc(false) }, 400000);
        on.setMix({ gc(true) }, 400000);
        check(masterPeak(off.master(), -1) > 0.02, "puerta por clip: señal floja presente sin puerta");
        check(peakRange(on.master(), 0.15, 0.4) < 0.005, "puerta por clip cierra bajo el umbral");
        QFile::remove(quiet);
    }

    // 17) Horneado POR BLOQUES: un clip que abarca varios bloques de 1 s se produce
    //     continuo (el tono está presente en los tres bloques, sin huecos en los límites).
    {
        const QString t3 = dir + "/pvs_t3.wav";
        writeToneWav(t3, 0.5, 3.2);
        AudioPlayer p;
        p.setMix({ clip(t3, 3, 0, 3000000, 0, 1.0, 1.0, 0.0) }, 3000000);   // 3 s (3 bloques)
        check(p.master().size() / 4 == 3 * 48000, "master de 3 s = 3 bloques, longitud correcta");
        check(peakRange(p.master(), 0.2, 0.8) > 0.4
                  && peakRange(p.master(), 1.2, 1.8) > 0.4    // cruza el límite en 1 s
                  && peakRange(p.master(), 2.2, 2.8) > 0.4,   // cruza el límite en 2 s
              "tono continuo a través de los límites de bloque");
        QFile::remove(t3);
    }

    // 18) Automatización de fx por keyframes: rampa del EQ Medios de −12 a +12 dB sobre un
    //     tono de 1 kHz → el pico sube de principio a fin del clip.
    {
        const QString ta = dir + "/pvs_auto.wav";
        writeToneWav(ta, 0.4, 3.2);
        AudioMixClip c = clip(ta, 3, 0, 3000000, 0, 1.0, 1.0, 0.0);
        c.clipEqOn = true;
        c.clipEqMidKf = { { 0, -12.0 }, { 3000000, 12.0 } };
        AudioPlayer p;
        p.setMix({ c }, 3000000);
        const double early = peakRange(p.master(), 0.1, 0.5);   // medios atenuados
        const double late  = peakRange(p.master(), 2.4, 2.9);   // medios realzados
        check(late > early * 1.5, "automatización de EQ: el pico sube de principio a fin");
        QFile::remove(ta);
    }

    // 19) Automatización del compresor: rampa del umbral de 0 a −30 dB sobre una señal
    //     fuerte → el pico baja de principio a fin.
    {
        const QString tc = dir + "/pvs_autoc.wav";
        writeToneWav(tc, 0.5, 3.2);
        AudioMixClip c = clip(tc, 3, 0, 3000000, 0, 1.0, 1.0, 0.0);
        c.clipCompOn = true; c.clipCompRatio = 8.0;
        c.clipCompThreshKf = { { 0, 0.0 }, { 3000000, -30.0 } };
        AudioPlayer p;
        p.setMix({ c }, 3000000);
        const double early = peakRange(p.master(), 0.1, 0.5);   // umbral alto → sin comprimir
        const double late  = peakRange(p.master(), 2.4, 2.9);   // umbral bajo → comprimido
        check(late < early * 0.7, "automatización del compresor: el pico baja de principio a fin");
        QFile::remove(tc);
    }

    QFile::remove(tone);
    QFile::remove(sil);
    qInfo("[AUDIO-SELFTEST] %d fallo(s)", failures);
    return failures == 0 ? 0 : 1;
}
