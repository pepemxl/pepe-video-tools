#pragma once

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QVector>

#include "../app/timelinemodel.h" // TimelineModel::RenderClip
#include "videoframe.h"

class FrameGrabber;
class QThread;
class QTimer;

using RenderClipList = QVector<TimelineModel::RenderClip>;

// Una capa del PROGRAMA lista para componer en GPU: el fotograma decodificado como
// VideoFrame (textura NV12 zero-copy, planos I420 o RGBA; los tiles de título y los
// rellenos viajan en `vf.rgba`) más los parámetros del clip (transform, color,
// opacidad, wipe) que el shader/nodo aplican.
struct ProgramLayer {
    VideoFrame vf;
    TimelineModel::RenderClip rc;
    // true = fotograma real de media: se etalona en el shader y usa el transform
    // completo; false = tile de título o relleno (identidad, lienzo completo).
    bool isVideoFrame = false;
};

// Instantánea de todas las capas activas en el playhead, con el lienzo de
// referencia en el que están expresadas las fracciones (posX/posY, wipe…).
struct ProgramLayers {
    QSize canvas;
    QVector<ProgramLayer> layers;
    bool hasContent = false;
};
Q_DECLARE_METATYPE(ProgramLayers)

// Worker de composición: vive en un hilo de trabajo propio, posee los FrameGrabber
// (cache por ruta) y produce las capas del fotograma (composición en GPU) y, si el
// análisis está activo, también el fotograma compuesto por CPU (scopes/still).
// Solo recibe datos por valor (RenderClipList), nunca toca el TimelineModel.
class CompositorWorker : public QObject
{
    Q_OBJECT
public:
    explicit CompositorWorker(QSize outSize, QObject *parent = nullptr);
    ~CompositorWorker() override;

    // Compone el fotograma de forma sincrónica (sin emitir). Público para pruebas,
    // el exportador (offline), los scopes y el botón de guardar fotograma.
    QImage renderFrame(const RenderClipList &clips, bool &hasContent);

    // Decodifica las capas del playhead sin componer (la GPU las compone).
    ProgramLayers renderLayers(const RenderClipList &clips);

    // Aplica corrección de color primaria (lift/gamma/gain + temp/tint/sat) in situ,
    // por LUT de canal. Público y estático para pruebas.
    static void gradeImage(QImage &img, const TimelineModel::Color &c);

    // Dibuja el título de texto de un clip (kind == "title") con su transformación.
    void drawTitle(QPainter &p, const TimelineModel::RenderClip &rc) const;

    // El fotograma CPU (scopes) se produce solo si el análisis está activo.
    void setAnalysisActive(bool on) { m_analysis.storeRelaxed(on ? 1 : 0); }

public slots:
    void composeFrame(const RenderClipList &clips);
    // Adopta el ID3D11Device del scene graph: los grabbers pasan a decodificar
    // zero-copy (los ya abiertos se reabren en el siguiente acceso).
    void adoptDevice(void *d3dDevice);

signals:
    void frameReady(const QImage &image, bool hasContent);
    void layersReady(const ProgramLayers &layers);

private:
    // Un decodificador por CLIP (clave ruta+clipId): dos clips del mismo archivo
    // en tiempos distintos conservan cada uno su decode secuencial y su caché.
    FrameGrabber *grabberFor(const QString &path, quint64 clipId);

    QHash<QString, FrameGrabber *> m_grabbers;
    QSize m_outSize;
    QAtomicInt m_analysis;    // 1 = componer también por CPU (scopes visibles)
    bool m_gpuLayers = true;  // false (PVS_GPU_PROG=0) = solo la ruta CPU clásica
    void *m_extDev = nullptr; // ID3D11Device* adoptado (referencia de los grabbers)
};

// Compositor multicapa del monitor de PROGRAMA (fachada en el hilo de GUI).
// Mantiene el reloj de reproducción y el estado; en cada composición toma una
// instantánea de los clips activos (TimelineModel::clipsAt) y la envía al worker.
// Control busy/pending: como mucho una composición en vuelo; si llega otra petición
// mientras el worker trabaja, se recompone al terminar con el playhead más reciente
// (descarte de fotogramas, latencia acotada).
class Compositor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY hasContentChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    // Con los Scopes visibles, el worker compone también por CPU (los alimenta).
    Q_PROPERTY(bool analysisActive READ analysisActive WRITE setAnalysisActive
               NOTIFY analysisActiveChanged)
public:
    explicit Compositor(QObject *parent = nullptr);
    ~Compositor() override;

    void setTimeline(TimelineModel *timeline);
    bool hasContent() const { return m_hasContent; }
    bool playing() const { return m_playing; }
    bool analysisActive() const { return m_analysisActive; }
    void setAnalysisActive(bool on);

    // Adopta el ID3D11Device del scene graph (lo invoca la VideoSurface del
    // PROGRAMA desde el hilo de render; se reenvía al hilo del worker).
    Q_INVOKABLE void adoptGraphicsDevice(void *d3dDevice);

    // Transporte del monitor de PROGRAMA (reproduce la timeline en tiempo real).
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();

    // Cadencia del reloj de reproducción = fps de la secuencia (evita el
    // judder de muestrear a un intervalo fijo distinto del material).
    void setFrameRate(double fps);

    // Botón ● del monitor: guarda el fotograma actual del PROGRAMA como PNG
    // (diálogo nativo de guardado). Sin efecto si aún no hay fotograma.
    Q_INVOKABLE void saveStillDialog();
    // Núcleo sin diálogo (pruebas): guarda el último fotograma en `path`.
    Q_INVOKABLE bool saveStill(const QString &path);

signals:
    void frameReady(const QImage &image);
    void layersReady(const ProgramLayers &layers);
    void hasContentChanged();
    void playingChanged();
    void analysisActiveChanged();

    // Peticiones al worker (conexiones encoladas entre hilos).
    void requestCompose(const RenderClipList &clips);
    void requestAdoptDevice(void *d3dDevice);

private:
    void scheduleComposite();
    void requestComposite();
    void onWorkerFrame(const QImage &image, bool hasContent);
    void onWorkerLayers(const ProgramLayers &layers);
    void tick();

    TimelineModel *m_timeline = nullptr;
    QThread *m_thread = nullptr;
    CompositorWorker *m_worker = nullptr;

    QTimer *m_debounce = nullptr;
    QTimer *m_clock = nullptr;         // reloj de reproducción (~30 Hz)
    QElapsedTimer m_wall;              // tiempo real transcurrido
    qint64 m_lastWallMs = 0;

    QImage m_lastFrame;                // último fotograma compuesto (para el botón ●)
    bool m_hasContent = false;
    bool m_playing = false;
    bool m_busy = false;               // hay una composición en vuelo en el worker
    bool m_pending = false;            // llegó otra petición mientras estaba ocupado
    bool m_analysisActive = false;     // scopes visibles → CPU compone también
    bool m_gpuProg = true;             // false (PVS_GPU_PROG=0) = ruta CPU clásica
};
