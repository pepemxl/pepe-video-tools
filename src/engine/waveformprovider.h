#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

class QThread;

// Worker de formas de onda (hilo propio): decodifica el audio completo de un
// archivo con FFmpeg y produce su envolvente de pico mono (100 valores/s).
class WaveformWorker : public QObject
{
    Q_OBJECT
public:
    // Decodifica el audio de `path` a una envolvente de pico mono normalizada
    // 0..1 (kEnvPerSec valores por segundo). Vacía si no hay audio o falla.
    static QVector<float> analyzeFile(const QString &path);

    static constexpr int kOutRate = 48000;
    static constexpr int kEnvHop = 480;                    // 10 ms @ 48 kHz
    static constexpr int kEnvPerSec = kOutRate / kEnvHop;  // 100 valores/s

public slots:
    void analyze(const QString &path);

signals:
    void envelopeReady(const QString &path, const QVector<float> &envelope);
};

// Fachada expuesta a QML (singleton PepeVideo.Waveforms). Cachea la envolvente
// de cada archivo; peaks() devuelve la porción visible de un clip re-muestreada
// a `bins` columnas. Si el archivo aún no está decodificado devuelve una lista
// vacía, encola el análisis en el worker y emite ready(path) al terminar.
class WaveformProvider : public QObject
{
    Q_OBJECT
public:
    explicit WaveformProvider(QObject *parent = nullptr);
    ~WaveformProvider() override;

    // Envolvente visible de un clip: la ventana de origen
    // [inSec, inSec + durSec·speed] repartida en `bins` picos 0..1.
    // Lista vacía = sin datos (decodificación pendiente o archivo sin audio).
    Q_INVOKABLE QVariantList peaks(const QString &path, double inSec, double durSec,
                                   double speed, int bins);

signals:
    void ready(const QString &path);           // la envolvente de `path` ya está en caché
    void requestAnalyze(const QString &path);  // interno → worker (conexión encolada)

private:
    QThread *m_thread = nullptr;
    WaveformWorker *m_worker = nullptr;
    QHash<QString, QVector<float>> m_cache;  // por archivo; vacía = sin audio (no reintentar)
    QSet<QString> m_pending;                 // análisis en curso (evita encolar duplicados)
};

// Auto-test (PVS_WAVE_SELFTEST). Devuelve 0 (OK) / 1 (fallo) / -1 (no solicitado).
int runWaveformSelfTestIfRequested();
