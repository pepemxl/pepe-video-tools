#pragma once

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVariantList>
#include <QVector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
class QAudioSink;
class QIODevice;
class QThread;
class QTimer;

// Un clip con audio para la mezcla (instantánea desde TimelineModel). Los tiempos van en µs.
struct AudioMixClip {
    QString mediaPath;
    int trackIndex = 0;
    qint64 startUs = 0;      // inicio en la línea de tiempo
    qint64 durationUs = 0;   // duración en la línea de tiempo
    qint64 inUs = 0;         // punto de entrada en el origen
    double speed = 1.0;
    double gain = 1.0;
    double pan = 0.0;        // -1 izq … +1 der
    bool mute = false;
    QVector<QPair<qint64, double>> gainKf; // automatización de ganancia (sourceUs, valor)
    QVector<QPair<qint64, double>> panKf;  // automatización de pan (sourceUs, valor)
    // Efectos de la pista (iguales para todos sus clips): el horneado agrupa por pista
    // y aplica EQ de 3 bandas + compresor al submix de la pista.
    bool eqOn = false;
    double eqLowDb = 0.0, eqMidDb = 0.0, eqHighDb = 0.0;
    bool compOn = false;
    double compThreshDb = -18.0, compRatio = 2.0, compMakeupDb = 0.0;
    // Procesadores adicionales de pista (puerta de ruido · de-esser · reverb).
    bool gateOn = false; double gateThreshDb = -40.0;
    bool deEssOn = false; double deEssThreshDb = -24.0;
    bool reverbOn = false; double reverbMix = 0.25, reverbSize = 0.5;
    // Efectos POR CLIP (aplicados al PCM del propio clip, antes del submix de pista).
    bool clipEqOn = false;
    double clipEqLowDb = 0.0, clipEqMidDb = 0.0, clipEqHighDb = 0.0;
    bool clipCompOn = false;
    double clipCompThreshDb = -18.0, clipCompRatio = 2.0, clipCompMakeupDb = 0.0;
    bool clipGateOn = false; double clipGateThreshDb = -40.0;
    bool clipDeEssOn = false; double clipDeEssThreshDb = -24.0;
    bool clipReverbOn = false; double clipReverbMix = 0.25, clipReverbSize = 0.5;
    // Automatización por keyframes (sourceUs, valor): EQ, reverb, compresor, puerta, de-esser.
    QVector<QPair<qint64, double>> clipEqLowKf, clipEqMidKf, clipEqHighKf, clipReverbMixKf;
    QVector<QPair<qint64, double>> clipCompThreshKf, clipCompRatioKf, clipCompMakeupKf,
                                   clipGateThreshKf, clipDeEssThreshKf;
};
Q_DECLARE_METATYPE(AudioMixClip)

// Worker de audio (hilo propio): hornea la mezcla multi-clip del timeline en un master
// PCM (48 kHz estéreo S16), lo reproduce por QAudioSink en sincronía con el playhead y
// calcula medidores de pico por pista + LUFS integrado del master.
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    // --- Primitivas de decodificación (sincrónicas; usadas por el horneado y las pruebas) ---
    bool openSource(const QString &path);
    void closeSource();
    QByteArray decodeChunk(double &peakL, double &peakR); // S16 estéreo, bloque a bloque
    void seekMs(qint64 ms);                               // reposiciona el decodificador
    // Decodifica un clip completo a float estéreo intercalado (48 kHz), sin ganancia/pan,
    // aplicando el remapeo de velocidad. Longitud = outSamples*2.
    QVector<float> decodeClipFloat(const AudioMixClip &clip);

    // Estado del horneado (público para pruebas).
    const QByteArray &master() const { return m_master; }
    double lufs() const { return m_lufs; }
    int trackCount() const { return int(m_env.size()); }
    double envMax(int trackIndex) const; // pico máximo de la envolvente de una pista
    double lufsShortMax() const;         // máximo de la envolvente de loudness a corto plazo

public slots:
    // masterGain/masterPan aplican la ganancia y el pan del bus MAIN al master final;
    // limiterOn/ceilingDb aplican un limitador de pico al master.
    void setMix(const QVector<AudioMixClip> &clips, qint64 endUs,
                double masterGain = 1.0, double masterPan = 0.0,
                bool limiterOn = false, double ceilingDb = -1.0);
    void startAt(qint64 startMs);
    void stop();

signals:
    void meters(const QVariantList &trackPeaks, double masterL, double masterR, double shortLufs);
    void mixReady(double lufs);
    void finished();

private:
    void pump();
    bool ensureSwr();
    void bake(const QVector<AudioMixClip> &clips, qint64 endUs,
              double masterGain, double masterPan, bool limiterOn, double ceilingDb);
    // Filtra el master con K-weighting: devuelve el LUFS integrado (puerta) y llena la
    // envolvente de loudness a corto plazo (ventana de 400 ms) por hop de envolvente.
    static double kWeightAnalyze(const QByteArray &masterS16, int rate,
                                 int envHop, QVector<float> &shortEnv);
    QVector<float> decodeCached(const AudioMixClip &clip); // decodeClipFloat con caché

    // Decodificador abierto (para openSource/decodeChunk/decodeClipFloat).
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwrContext *m_swr = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;
    int m_audioIndex = -1;
    double m_timeBaseSec = 0.0;
    bool m_eof = false;

    // Mezcla horneada.
    QByteArray m_master;                 // PCM S16 estéreo intercalado a 48 kHz
    QVector<QVector<float>> m_env;       // envolvente de pico por pista (hop de 10 ms)
    QVector<float> m_lufsEnv;            // loudness a corto plazo por hop (para el medidor en vivo)
    double m_lufs = -70.0;
    QHash<QString, QVector<float>> m_clipCache; // PCM decodificado por clip (evita re-decodificar)

    // Reproducción.
    QAudioSink *m_sink = nullptr;
    QIODevice *m_dev = nullptr;
    QTimer *m_pump = nullptr;
    qint64 m_cursor = 0;                 // byte actual dentro de m_master

    static constexpr int kOutRate = 48000;
    static constexpr int kEnvHop = 480;  // 10 ms @ 48 kHz
};

// Fachada expuesta a QML (singleton PepeVideo.Audio). Gestiona el hilo del worker,
// mantiene la lista de clips de la mezcla y expone niveles/estado.
class AudioEngine : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double peakL READ peakL NOTIFY levelChanged)
    Q_PROPERTY(double peakR READ peakR NOTIFY levelChanged)
    Q_PROPERTY(QVariantList trackPeaks READ trackPeaks NOTIFY levelChanged)
    Q_PROPERTY(double lufs READ lufs NOTIFY lufsChanged)
    Q_PROPERTY(double lufsShort READ lufsShort NOTIFY levelChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
public:
    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    // Fija la mezcla (lista de clips + fin del contenido + ajustes del bus MAIN).
    // Rehornea en el worker.
    void setMixData(const QVector<AudioMixClip> &clips, qint64 endUs,
                    double masterGain = 1.0, double masterPan = 0.0,
                    bool limiterOn = false, double ceilingDb = -1.0);

    double peakL() const { return m_peakL; }
    double peakR() const { return m_peakR; }
    QVariantList trackPeaks() const { return m_trackPeaks; }
    double lufs() const { return m_lufs; }
    double lufsShort() const { return m_lufsShort; }
    bool playing() const { return m_playing; }
    Q_INVOKABLE double trackPeak(int trackIndex) const;

    Q_INVOKABLE void playFrom(qint64 ms);
    Q_INVOKABLE void stop();

signals:
    void levelChanged();
    void lufsChanged();
    void playingChanged();

    void requestMix(const QVector<AudioMixClip> &clips, qint64 endUs,
                    double masterGain, double masterPan, bool limiterOn, double ceilingDb);
    void requestStart(qint64 startMs);
    void requestStop();

private:
    QThread *m_thread = nullptr;
    AudioPlayer *m_player = nullptr;
    QTimer *m_rebake = nullptr;                 // coalesce ediciones rápidas antes de rehornear
    QVector<AudioMixClip> m_pendingClips;
    qint64 m_pendingEnd = 0;
    double m_pendingMasterGain = 1.0;
    double m_pendingMasterPan = 0.0;
    bool m_pendingLimiterOn = false;
    double m_pendingCeilingDb = -1.0;
    double m_peakL = 0.0, m_peakR = 0.0;
    QVariantList m_trackPeaks;
    double m_lufs = -70.0;
    double m_lufsShort = -70.0;
    bool m_playing = false;
};

// Auto-test de audio (PVS_AUDIO_SELFTEST). Devuelve 0 (OK) / 1 (fallo) / -1 (no solicitado).
int runAudioSelfTestIfRequested();
