#include "timelinemodel.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUndoCommand>
#include <QVariantMap>
#include <algorithm>
#include <functional>
#include <limits>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commdlg.h>
#endif

// ---- Comando genérico (redo/undo como lambdas) ----
class TimelineCommand : public QUndoCommand
{
public:
    TimelineCommand(const QString &text, std::function<void()> redoFn, std::function<void()> undoFn)
        : m_redo(std::move(redoFn)), m_undo(std::move(undoFn)) { setText(text); }
    void redo() override { m_redo(); }
    void undo() override { m_undo(); }
private:
    std::function<void()> m_redo, m_undo;
};

TimelineModel::TimelineModel(QObject *parent) : QObject(parent)
{
    seed();
    // Toda edición estructural pasa por la pila de undo → señal de documento editado.
    connect(&m_undo, &QUndoStack::indexChanged, this, &TimelineModel::edited);
    runSelfTestIfRequested();
}

void TimelineModel::seed()
{
    m_tracks = {
        { "V3", "video", "#8a6bc0", 44 },
        { "V2", "video", "#5b8dd6", 56 },
        { "V1", "video", "#5b8dd6", 60 },
        { "A1", "audio", "#4a9e6b", 48 },
        { "A2", "audio", "#8a6bc0", 48 },
        { "A3", "audio", "#4a9e6b", 40 },
    };

    const qint64 T = m_totalUs;
    auto us = [T](double frac) { return qint64(frac * T); };
    auto add = [this](int track, const QString &name, const QString &kind, const QString &fill,
                      const QString &border, const QString &wav, qint64 start, qint64 dur) {
        m_clips.push_back({ m_nextId++, track, name, kind, fill, border, wav, start, dur, 0, QString() });
    };

    // V3 títulos
    add(0, "T · Mercado de Abastos", "title", "#4a3f6b", "#6a5a94", "", us(0.24), us(0.20));
    // V2 b-roll
    add(1, "Drone_zocalo",       "video", "#2f5560", "#3f7d8f", "", us(0.06), us(0.16));
    add(1, "B009_puesto ◧ 45%",  "video", "#2f5560", "#3f7d8f", "", us(0.46), us(0.22));
    // V1 principal
    add(2, "A012_entrevista",    "video", "#345066", "#47708f", "", us(0.00), us(0.20));
    add(2, "A017_mercado",       "video", "#345066", "#47708f", "", us(0.20), us(0.26));
    add(2, "A012_entrevista · 02","video", "#345066", "#47708f", "", us(0.46), us(0.30));
    add(2, "A020_calle",         "video", "#345066", "#47708f", "", us(0.76), us(0.24));
    // A1 diálogo
    add(3, "Diálogo A",          "audio", "#2d5540", "#3c7052", "#5fbf87", us(0.00), us(0.46));
    add(3, "",                   "audio", "#2d5540", "#3c7052", "#5fbf87", us(0.46), us(0.30));
    // A2 música
    add(4, "Musica_intro ▼ automatización", "audio", "#4a3f6b", "#6a5a94", "#b39ad6", us(0.00), us(0.92));
    // A3 ambiente
    add(5, "Ambiente_mercado",   "audio", "#2d5540", "#3c7052", "#4d9970", us(0.06), us(0.88));

    // Texto de los clips de título del seed (lower third de ejemplo).
    for (Clip &c : m_clips)
        if (c.kind == QLatin1String("title")) {
            c.title.text = QStringLiteral("Mercado de Abastos");
            c.title.bar = true;
            c.title.sizePt = 0.075;
            c.transform.posY = 0.28; // tercio inferior
        }

    m_playheadUs = us(0.52);

    // Hook de prueba: asigna un archivo real a los clips de V1 (pista 2) para
    // poder verificar el compositor con vídeo real. PVS_TL_MEDIA=<ruta>.
    const QString demoMedia = qEnvironmentVariable("PVS_TL_MEDIA");
    if (!demoMedia.isEmpty())
        for (Clip &c : m_clips)
            if (c.trackIndex == 2)
                c.mediaPath = demoMedia;

    // Hook de prueba: asigna audio real a las pistas A1–A3 (índices 3–5) para verificar
    // la mezcla multi-clip. PVS_TL_AUDIO=<ruta>.
    const QString demoAudio = qEnvironmentVariable("PVS_TL_AUDIO");
    if (!demoAudio.isEmpty())
        for (Clip &c : m_clips)
            if (c.trackIndex >= 3)
                c.mediaPath = demoAudio;
}

QVector<TimelineModel::RenderClip> TimelineModel::clipsAt(qint64 us) const
{
    QVector<RenderClip> out;

    // Resuelve un clip a RenderClip: evalúa keyframes en srcUs y multiplica la opacidad
    // por opFactor (crossfade). El worker no necesita las listas de keyframes.
    auto resolved = [](const Clip &c, qint64 nowUs, double opFactor) -> RenderClip {
        const qint64 srcUs = c.inUs + qint64((nowUs - c.startUs) * c.speed);
        Transform rt = c.transform;
        rt.posX = evalKf(c.transform.kfPosX, c.transform.posX, srcUs);
        rt.posY = evalKf(c.transform.kfPosY, c.transform.posY, srcUs);
        rt.scale = evalKf(c.transform.kfScale, c.transform.scale, srcUs);
        rt.rotation = evalKf(c.transform.kfRotation, c.transform.rotation, srcUs);
        rt.opacity = evalKf(c.transform.kfOpacity, c.transform.opacity, srcUs) * opFactor;
        rt.kfPosX.clear(); rt.kfPosY.clear(); rt.kfScale.clear();
        rt.kfRotation.clear(); rt.kfOpacity.clear();
        return { c.trackIndex, c.kind, c.fill, c.mediaPath, srcUs, rt, c.color, c.title };
    };

    // Pistas de vídeo de abajo (índice mayor, V1) hacia arriba (índice menor, V3),
    // para pintarlas en ese orden (las de arriba tapan a las de abajo).
    for (int t = m_tracks.size() - 1; t >= 0; --t) {
        if (m_tracks.at(t).kind != QLatin1String("video"))
            continue;
        if (m_tracks.at(t).hidden)   // pista de vídeo oculta (👁): no se compone
            continue;

        // Clips activos en la pista, ordenados por inicio.
        QVector<const Clip *> active;
        for (const Clip &c : m_clips)
            if (c.trackIndex == t && us >= c.startUs && us < c.startUs + c.durationUs)
                active.push_back(&c);
        if (active.isEmpty())
            continue;
        std::sort(active.begin(), active.end(),
                  [](const Clip *a, const Clip *b) { return a->startUs < b->startUs; });

        // Sin solape: un clip. Con solape: el saliente (A) a plena opacidad y el entrante
        // (B) con opacidad de crossfade f = avance dentro de la región de solape.
        const Clip *B = active.last();
        const Clip *A = active.size() >= 2 ? active.at(active.size() - 2) : nullptr;
        double f = 1.0;
        if (A) {
            const qint64 ovStart = B->startUs;                    // inicio del solape
            const qint64 ovEnd = A->startUs + A->durationUs;      // fin del solape (fin de A)
            if (ovEnd > ovStart)
                f = qBound(0.0, double(us - ovStart) / double(ovEnd - ovStart), 1.0);
            out.push_back(resolved(*A, us, 1.0));                 // saliente debajo
        }
        out.push_back(resolved(*B, us, f));                       // entrante encima (crossfade)
    }

    // Subtítulo activo: se pinta encima de todo como un título en el tercio inferior.
    if (m_subsEnabled) {
        for (const Subtitle &s : m_subtitles) {
            if (us >= s.startUs && us < s.endUs) {
                RenderClip rc{};
                rc.kind = QStringLiteral("title");
                rc.sourceUs = us;
                rc.transform.posY = 0.40;             // tercio inferior
                rc.title.text = s.text;
                rc.title.sizePt = 0.055;
                rc.title.color = QStringLiteral("#ffffff");
                rc.title.align = 1;                   // centrado
                rc.title.bar = true;
                rc.title.barColor = QStringLiteral("#b0000000");
                out.push_back(rc);
                break;                                // un subtítulo a la vez
            }
        }
    }
    return out;
}

void TimelineModel::setClipMedia(quint64 id, const QString &path)
{
    const int i = indexOfClip(id);
    if (i < 0 || m_clips[i].mediaPath == path)
        return;
    m_clips[i].mediaPath = path;
    emit changed();
    emit edited();
}

int TimelineModel::indexOfClip(quint64 id) const
{
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips.at(i).id == id)
            return i;
    return -1;
}

void TimelineModel::doInsert(const Clip &c, int at)
{
    at = qBound(0, at, int(m_clips.size()));
    m_clips.insert(at, c);
}

TimelineModel::Clip TimelineModel::doRemoveAt(int at)
{
    Clip c = m_clips.at(at);
    m_clips.remove(at);
    return c;
}

QVariantList TimelineModel::tracks() const
{
    QVariantList out;
    for (int t = 0; t < m_tracks.size(); ++t) {
        const Track &tr = m_tracks.at(t);
        QVariantList clips;
        for (const Clip &c : m_clips) {
            if (c.trackIndex != t)
                continue;
            QVariantMap m;
            m["id"] = QVariant::fromValue(c.id);
            m["x"] = m_totalUs > 0 ? double(c.startUs) / m_totalUs : 0.0;
            m["w"] = m_totalUs > 0 ? double(c.durationUs) / m_totalUs : 0.0;
            m["name"] = c.name;
            m["kind"] = c.kind;
            m["fill"] = c.fill;
            m["border"] = c.border;
            m["wav"] = c.wav;
            // Datos para la forma de onda real (Waveforms.peaks en QML).
            m["mediaPath"] = c.mediaPath;
            m["inSec"] = c.inUs / 1e6;
            m["durSec"] = c.durationUs / 1e6;
            m["speed"] = c.speed;
            m["selected"] = (c.id == m_selectedId);
            clips.append(m);
        }
        QVariantMap tm;
        tm["name"] = tr.name;
        tm["kind"] = tr.kind;
        tm["idColor"] = tr.idColor;
        tm["height"] = tr.height;
        tm["index"] = t;
        tm["mute"] = tr.mute;
        tm["solo"] = tr.solo;
        tm["hidden"] = tr.hidden;
        tm["locked"] = tr.locked;
        tm["clips"] = clips;
        out.append(tm);
    }
    return out;
}

QVariantList TimelineModel::markers() const
{
    QVariantList out;
    for (const Marker &mk : m_markers) {
        QVariantMap m;
        m["x"] = m_totalUs > 0 ? double(mk.timeUs) / m_totalUs : 0.0;
        m["color"] = mk.color;
        m["note"] = mk.note;
        out.append(m);
    }
    return out;
}

void TimelineModel::addMarkerAtPlayhead()
{
    addMarkerAtFraction(m_totalUs > 0 ? double(m_playheadUs) / m_totalUs : 0.0);
}

void TimelineModel::addMarkerAtFraction(double f)
{
    const qint64 t = qBound<qint64>(0, qint64(f * m_totalUs), m_totalUs);
    // Evita duplicar un marcador prácticamente en el mismo punto.
    const qint64 tol = qint64(m_totalUs * 0.004);
    for (const Marker &mk : m_markers)
        if (qAbs(mk.timeUs - t) < tol)
            return;
    m_markers.push_back({ t, QStringLiteral("#e2a24b"), QString() });
    std::sort(m_markers.begin(), m_markers.end(),
              [](const Marker &a, const Marker &b) { return a.timeUs < b.timeUs; });
    emit markersChanged();
}

void TimelineModel::removeMarkerNear(double f)
{
    const qint64 t = qint64(f * m_totalUs);
    const qint64 tol = qint64(m_totalUs * 0.01);
    for (int i = 0; i < m_markers.size(); ++i) {
        if (qAbs(m_markers.at(i).timeUs - t) < tol) {
            m_markers.remove(i);
            emit markersChanged();
            return;
        }
    }
}

void TimelineModel::selectClip(quint64 id)
{
    if (m_selectedId == id)
        return;
    m_selectedId = id;
    emit changed();          // actualiza el resalte de selección en la timeline
    emit selectionChanged(); // refresca el Inspector
}

// ---- Keyframes: helpers ----
double TimelineModel::evalKf(const QVector<Keyframe> &kf, double staticVal, qint64 sourceUs)
{
    if (kf.isEmpty())
        return staticVal;
    if (sourceUs <= kf.first().sourceUs)
        return kf.first().value;
    if (sourceUs >= kf.last().sourceUs)
        return kf.last().value;
    for (int i = 1; i < kf.size(); ++i) {
        if (sourceUs <= kf[i].sourceUs) {
            const Keyframe &a = kf[i - 1];
            const Keyframe &b = kf[i];
            const double span = double(b.sourceUs - a.sourceUs);
            const double t = span > 0 ? double(sourceUs - a.sourceUs) / span : 0.0;
            return a.value + (b.value - a.value) * t;
        }
    }
    return kf.last().value;
}

QVector<TimelineModel::Keyframe> *TimelineModel::kfVec(Transform &tf, const QString &prop, double *&staticOut)
{
    if (prop == QLatin1String("posX"))     { staticOut = &tf.posX;     return &tf.kfPosX; }
    if (prop == QLatin1String("posY"))     { staticOut = &tf.posY;     return &tf.kfPosY; }
    if (prop == QLatin1String("scale"))    { staticOut = &tf.scale;    return &tf.kfScale; }
    if (prop == QLatin1String("rotation")) { staticOut = &tf.rotation; return &tf.kfRotation; }
    if (prop == QLatin1String("opacity"))  { staticOut = &tf.opacity;  return &tf.kfOpacity; }
    staticOut = nullptr;
    return nullptr;
}

const QVector<TimelineModel::Keyframe> *TimelineModel::kfVec(const Transform &tf, const QString &prop) const
{
    if (prop == QLatin1String("posX"))     return &tf.kfPosX;
    if (prop == QLatin1String("posY"))     return &tf.kfPosY;
    if (prop == QLatin1String("scale"))    return &tf.kfScale;
    if (prop == QLatin1String("rotation")) return &tf.kfRotation;
    if (prop == QLatin1String("opacity"))  return &tf.kfOpacity;
    return nullptr;
}

void TimelineModel::bumpSelection()
{
    // Todos los llamadores de bumpSelection() son EDICIONES del clip seleccionado
    // (transform/color/audio/título/velocidad); seleccionar no pasa por aquí.
    emit selectionChanged();
    emit edited();
}

// ---- Transformación del clip seleccionado (valores evaluados en el playhead) ----
QString TimelineModel::selectedName() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 ? m_clips[i].name : QString();
}
double TimelineModel::selOpacity() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 1.0;
    const Clip &c = m_clips[i];
    return evalKf(c.transform.kfOpacity, c.transform.opacity, srcAtPlayhead(c));
}
double TimelineModel::selScale() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 1.0;
    const Clip &c = m_clips[i];
    return evalKf(c.transform.kfScale, c.transform.scale, srcAtPlayhead(c));
}
double TimelineModel::selPosX() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 0.0;
    const Clip &c = m_clips[i];
    return evalKf(c.transform.kfPosX, c.transform.posX, srcAtPlayhead(c));
}
double TimelineModel::selPosY() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 0.0;
    const Clip &c = m_clips[i];
    return evalKf(c.transform.kfPosY, c.transform.posY, srcAtPlayhead(c));
}
double TimelineModel::selRotation() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 0.0;
    const Clip &c = m_clips[i];
    return evalKf(c.transform.kfRotation, c.transform.rotation, srcAtPlayhead(c));
}

// Aplica un valor a una propiedad: al keyframe del playhead si está animada, o al estático.
static void applyValue(TimelineModel::Transform &tf, QVector<TimelineModel::Keyframe> *kf,
                       double *staticVal, qint64 src, double v)
{
    Q_UNUSED(tf);
    if (kf->isEmpty()) {
        *staticVal = v;
        return;
    }
    // Actualiza el keyframe cercano al playhead o inserta uno nuevo, manteniendo el orden.
    const qint64 tol = 20000; // 20 ms
    for (int i = 0; i < kf->size(); ++i) {
        if (qAbs(kf->at(i).sourceUs - src) < tol) { (*kf)[i].value = v; return; }
    }
    kf->push_back({ src, v });
    std::sort(kf->begin(), kf->end(),
              [](const TimelineModel::Keyframe &a, const TimelineModel::Keyframe &b) { return a.sourceUs < b.sourceUs; });
}

void TimelineModel::setSelOpacity(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(0.0, v, 1.0);
    double *sv = nullptr; auto *kf = kfVec(m_clips[i].transform, QStringLiteral("opacity"), sv);
    applyValue(m_clips[i].transform, kf, sv, srcAtPlayhead(m_clips[i]), v);
    bumpSelection();
}
void TimelineModel::setSelScale(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(0.05, v, 8.0);
    double *sv = nullptr; auto *kf = kfVec(m_clips[i].transform, QStringLiteral("scale"), sv);
    applyValue(m_clips[i].transform, kf, sv, srcAtPlayhead(m_clips[i]), v);
    bumpSelection();
}
void TimelineModel::setSelPosX(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(-2.0, v, 2.0);
    double *sv = nullptr; auto *kf = kfVec(m_clips[i].transform, QStringLiteral("posX"), sv);
    applyValue(m_clips[i].transform, kf, sv, srcAtPlayhead(m_clips[i]), v);
    bumpSelection();
}
void TimelineModel::setSelPosY(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(-2.0, v, 2.0);
    double *sv = nullptr; auto *kf = kfVec(m_clips[i].transform, QStringLiteral("posY"), sv);
    applyValue(m_clips[i].transform, kf, sv, srcAtPlayhead(m_clips[i]), v);
    bumpSelection();
}
void TimelineModel::setSelRotation(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    while (v > 180.0) v -= 360.0;
    while (v < -180.0) v += 360.0;
    double *sv = nullptr; auto *kf = kfVec(m_clips[i].transform, QStringLiteral("rotation"), sv);
    applyValue(m_clips[i].transform, kf, sv, srcAtPlayhead(m_clips[i]), v);
    bumpSelection();
}

// ---- Corrección de color del clip seleccionado ----
double TimelineModel::selLiftX() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.liftX : 0.0; }
double TimelineModel::selLiftY() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.liftY : 0.0; }
double TimelineModel::selGammaX() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.gammaX : 0.0; }
double TimelineModel::selGammaY() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.gammaY : 0.0; }
double TimelineModel::selGainX() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.gainX : 0.0; }
double TimelineModel::selGainY() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.gainY : 0.0; }
double TimelineModel::selTemp() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.temp : 0.0; }
double TimelineModel::selTint() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.tint : 0.0; }
double TimelineModel::selSat() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].color.sat : 1.0; }

void TimelineModel::setSelLift(double x, double y)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color.liftX = qBound(-1.0, x, 1.0);
    m_clips[i].color.liftY = qBound(-1.0, y, 1.0);
    bumpSelection();
}
void TimelineModel::setSelGamma(double x, double y)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color.gammaX = qBound(-1.0, x, 1.0);
    m_clips[i].color.gammaY = qBound(-1.0, y, 1.0);
    bumpSelection();
}
void TimelineModel::setSelGain(double x, double y)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color.gainX = qBound(-1.0, x, 1.0);
    m_clips[i].color.gainY = qBound(-1.0, y, 1.0);
    bumpSelection();
}
void TimelineModel::setSelTemp(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color.temp = qBound(-1.0, v, 1.0);
    bumpSelection();
}
void TimelineModel::setSelTint(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color.tint = qBound(-1.0, v, 1.0);
    bumpSelection();
}
void TimelineModel::setSelSat(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color.sat = qBound(0.0, v, 2.0);
    bumpSelection();
}
void TimelineModel::resetSelColor()
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    m_clips[i].color = Color{};
    bumpSelection();
}

double TimelineModel::selSpeed() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 ? m_clips[i].speed : 1.0;
}
void TimelineModel::setSelSpeed(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(0.1, v, 8.0);
    if (m_clips[i].speed == v) return;
    m_clips[i].speed = v;
    bumpSelection();
}

// ---- Audio del clip seleccionado ----
bool TimelineModel::selHasAudio() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && !m_clips[i].mediaPath.isEmpty()
        && m_clips[i].kind != QLatin1String("title");
}
double TimelineModel::selAudioGain() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 1.0;
    const Clip &c = m_clips[i];
    return evalKf(c.audio.gainKf, c.audio.gain, srcAtPlayhead(c));
}
double TimelineModel::selPan() const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return 0.0;
    const Clip &c = m_clips[i];
    return evalKf(c.audio.panKf, c.audio.pan, srcAtPlayhead(c));
}
bool TimelineModel::selAudioMute() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].audio.mute;
}
// Aplica un valor a una automatización de audio: al keyframe del playhead si está animada
// (actualiza el cercano o inserta), o al valor estático si no hay keyframes.
static void applyAudioKf(QVector<TimelineModel::Keyframe> &kf, double &staticVal, qint64 src, double v)
{
    if (kf.isEmpty()) { staticVal = v; return; }
    const qint64 tol = 20000;
    for (int i = 0; i < kf.size(); ++i)
        if (qAbs(kf[i].sourceUs - src) < tol) { kf[i].value = v; return; }
    kf.push_back({ src, v });
    std::sort(kf.begin(), kf.end(),
              [](const TimelineModel::Keyframe &a, const TimelineModel::Keyframe &b) { return a.sourceUs < b.sourceUs; });
}

void TimelineModel::setSelAudioGain(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(0.0, v, 4.0);
    Clip &c = m_clips[i];
    applyAudioKf(c.audio.gainKf, c.audio.gain, srcAtPlayhead(c), v);
    bumpSelection();
    emit audioChanged();
}
void TimelineModel::setSelPan(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(-1.0, v, 1.0);
    Clip &c = m_clips[i];
    applyAudioKf(c.audio.panKf, c.audio.pan, srcAtPlayhead(c), v);
    bumpSelection();
    emit audioChanged();
}
void TimelineModel::setTrackMute(int trackIndex, bool m)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks[trackIndex].mute == m) return;
    m_tracks[trackIndex].mute = m;
    emit audioChanged();
}
void TimelineModel::setTrackSolo(int trackIndex, bool s)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks[trackIndex].solo == s) return;
    m_tracks[trackIndex].solo = s;
    emit audioChanged();
}
void TimelineModel::setTrackGain(int trackIndex, double gain)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    gain = qBound(0.0, gain, 4.0);  // hasta +12 dB
    if (qFuzzyCompare(m_tracks[trackIndex].gain, gain)) return;
    m_tracks[trackIndex].gain = gain;
    emit audioChanged();
}
void TimelineModel::setTrackPan(int trackIndex, double pan)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    pan = qBound(-1.0, pan, 1.0);
    if (qFuzzyCompare(m_tracks[trackIndex].pan, pan)) return;
    m_tracks[trackIndex].pan = pan;
    emit audioChanged();
}
void TimelineModel::setTrackHidden(int trackIndex, bool hidden)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks[trackIndex].hidden == hidden) return;
    m_tracks[trackIndex].hidden = hidden;
    emit changed();   // el compositor recompone al cambiar la estructura
    emit edited();
}
void TimelineModel::setTrackLocked(int trackIndex, bool locked)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks[trackIndex].locked == locked) return;
    m_tracks[trackIndex].locked = locked;
    emit changed();
    emit edited();
}

QVariantList TimelineModel::audioTracks() const
{
    QVariantList out;
    for (int t = 0; t < m_tracks.size(); ++t) {
        const Track &tr = m_tracks[t];
        out.append(QVariantMap{
            { "index", t }, { "name", tr.name }, { "kind", tr.kind },
            { "mute", tr.mute }, { "solo", tr.solo },
            { "gain", tr.gain }, { "pan", tr.pan } });
    }
    return out;
}
void TimelineModel::setSelAudioMute(bool m)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    if (m_clips[i].audio.mute == m) return;
    m_clips[i].audio.mute = m;
    bumpSelection();
    emit audioChanged();
}

// ---- Título del clip seleccionado ----
bool TimelineModel::selIsTitle() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].kind == QLatin1String("title");
}
QString TimelineModel::selTitleText() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 ? m_clips[i].title.text : QString();
}
double TimelineModel::selTitleSize() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 ? m_clips[i].title.sizePt : 0.09;
}
QString TimelineModel::selTitleColor() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 ? m_clips[i].title.color : QStringLiteral("#ffffff");
}
int TimelineModel::selTitleAlign() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 ? m_clips[i].title.align : 1;
}
bool TimelineModel::selTitleBar() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].title.bar;
}
void TimelineModel::setSelTitleText(const QString &text)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].title.text == text) return;
    m_clips[i].title.text = text;
    bumpSelection();   // recompón (el compositor escucha selectionChanged)
}
void TimelineModel::setSelTitleSize(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(0.02, v, 0.5);
    if (m_clips[i].title.sizePt == v) return;
    m_clips[i].title.sizePt = v;
    bumpSelection();
}
void TimelineModel::setSelTitleColor(const QString &color)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].title.color == color) return;
    m_clips[i].title.color = color;
    bumpSelection();
}
void TimelineModel::setSelTitleAlign(int align)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    align = qBound(0, align, 2);
    if (m_clips[i].title.align == align) return;
    m_clips[i].title.align = align;
    bumpSelection();
}
void TimelineModel::setSelTitleBar(bool bar)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].title.bar == bar) return;
    m_clips[i].title.bar = bar;
    bumpSelection();
}
void TimelineModel::addTitleAtPlayhead()
{
    Clip t;
    t.id = m_nextId++;
    t.trackIndex = 0;                 // V3
    t.name = QStringLiteral("Título");
    t.kind = QStringLiteral("title");
    t.fill = QStringLiteral("#4a3f6b");
    t.border = QStringLiteral("#6a5a94");
    t.startUs = m_playheadUs;
    t.durationUs = qMin(m_totalUs / 20, m_totalUs - m_playheadUs); // ~15 s por defecto
    if (t.durationUs < kMinClipUs) t.durationUs = kMinClipUs;
    t.inUs = 0;
    const quint64 id = t.id;
    m_undo.push(new TimelineCommand(
        QStringLiteral("Añadir título"),
        [this, t]() { doInsert(t, m_clips.size()); emit changed(); },
        [this, id]() { const int r = indexOfClip(id); if (r >= 0) doRemoveAt(r); emit changed(); }));
    selectClip(id);
}

quint64 TimelineModel::addMediaClip(const QString &path, const QString &name,
                                    const QString &kind, qint64 durationUs,
                                    int trackIndex, double startFraction)
{
    if (path.isEmpty()) return 0;
    const bool isAudio = (kind == QLatin1String("audio"));

    // Ajusta a una pista del tipo correcto (vídeo↔vídeo, audio↔audio).
    if (trackIndex < 0 || trackIndex >= m_tracks.size()
        || (m_tracks[trackIndex].kind == QLatin1String("audio")) != isAudio) {
        trackIndex = -1;
        for (int t = 0; t < m_tracks.size(); ++t)
            if ((m_tracks[t].kind == QLatin1String("audio")) == isAudio) { trackIndex = t; break; }
        if (trackIndex < 0) return 0;
    }

    qint64 dur = durationUs > 0 ? durationUs : 5LL * 1000000;
    dur = qMin(dur, m_totalUs);
    qint64 start = qMax<qint64>(0, qint64(startFraction * m_totalUs));
    if (start + dur > m_totalUs) start = qMax<qint64>(0, m_totalUs - dur);
    start = snapUs(start, 0);

    Clip c;
    c.id = m_nextId++;
    c.trackIndex = trackIndex;
    c.name = name.isEmpty() ? QFileInfo(path).fileName() : name;
    c.kind = isAudio ? QStringLiteral("audio") : QStringLiteral("video");
    c.fill = isAudio ? QStringLiteral("#2d5540") : QStringLiteral("#345066");
    c.border = isAudio ? QStringLiteral("#3c7052") : QStringLiteral("#47708f");
    c.wav = isAudio ? QStringLiteral("#5fbf87") : QString();
    c.startUs = start;
    c.durationUs = dur;
    c.inUs = 0;
    c.mediaPath = path;
    const quint64 id = c.id;
    m_undo.push(new TimelineCommand(
        QStringLiteral("Añadir clip"),
        [this, c]() { doInsert(c, m_clips.size()); emit changed(); emit audioChanged(); },
        [this, id]() { const int r = indexOfClip(id); if (r >= 0) doRemoveAt(r);
                       emit changed(); emit audioChanged(); }));
    selectClip(id);
    return id;
}

// ---- Subtítulos (.srt) ----
void TimelineModel::setSubtitlesEnabled(bool on)
{
    if (m_subsEnabled == on) return;
    m_subsEnabled = on;
    emit subtitlesChanged();
    emit playheadChanged();   // refresca activeSubtitle y recompón
}

QString TimelineModel::activeSubtitle() const
{
    if (!m_subsEnabled) return QString();
    for (const Subtitle &s : m_subtitles)
        if (m_playheadUs >= s.startUs && m_playheadUs < s.endUs)
            return s.text;
    return QString();
}

QVector<TimelineModel::Subtitle> TimelineModel::parseSrt(const QString &content)
{
    QVector<Subtitle> out;
    QString c = content;
    if (c.startsWith(QChar(0xFEFF))) c.remove(0, 1);            // BOM
    c.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    c.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    static const QRegularExpression blankLine(QStringLiteral("\n[ \t]*\n"));
    static const QRegularExpression tc(QStringLiteral(
        "(\\d{1,2}):(\\d{2}):(\\d{2})[,\\.](\\d{1,3})\\s*-->\\s*"
        "(\\d{1,2}):(\\d{2}):(\\d{2})[,\\.](\\d{1,3})"));

    auto toUs = [](int h, int m, int s, int ms) {
        return ((qint64(h) * 3600 + m * 60 + s) * 1000 + ms) * 1000LL;
    };

    const QStringList blocks = c.split(blankLine, Qt::SkipEmptyParts);
    for (const QString &b : blocks) {
        const QStringList lines = b.split(QLatin1Char('\n'));
        int tcLine = -1;
        QRegularExpressionMatch m;
        for (int i = 0; i < lines.size(); ++i) {
            m = tc.match(lines.at(i));
            if (m.hasMatch()) { tcLine = i; break; }
        }
        if (tcLine < 0) continue;
        // Milisegundos: rellena a la derecha hasta 3 dígitos (",5" = 500 ms).
        const qint64 st = toUs(m.captured(1).toInt(), m.captured(2).toInt(),
                               m.captured(3).toInt(), m.captured(4).leftJustified(3, '0').toInt());
        const qint64 en = toUs(m.captured(5).toInt(), m.captured(6).toInt(),
                               m.captured(7).toInt(), m.captured(8).leftJustified(3, '0').toInt());
        QStringList textLines;
        for (int i = tcLine + 1; i < lines.size(); ++i) textLines << lines.at(i);
        Subtitle s{ st, en, textLines.join(QLatin1Char('\n')).trimmed() };
        if (s.endUs > s.startUs && !s.text.isEmpty()) out.push_back(s);
    }
    std::sort(out.begin(), out.end(),
              [](const Subtitle &a, const Subtitle &b) { return a.startUs < b.startUs; });
    return out;
}

QString TimelineModel::serializeSrt(const QVector<Subtitle> &subs)
{
    auto fmt = [](qint64 us) {
        qint64 ms = us / 1000;
        const int h = int(ms / 3600000); ms %= 3600000;
        const int mi = int(ms / 60000);  ms %= 60000;
        const int s = int(ms / 1000);
        const int m = int(ms % 1000);
        return QStringLiteral("%1:%2:%3,%4")
            .arg(h, 2, 10, QChar('0')).arg(mi, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0')).arg(m, 3, 10, QChar('0'));
    };
    QString out;
    for (int i = 0; i < subs.size(); ++i) {
        out += QString::number(i + 1) + QLatin1Char('\n');
        out += fmt(subs.at(i).startUs) + QLatin1String(" --> ") + fmt(subs.at(i).endUs) + QLatin1Char('\n');
        out += subs.at(i).text + QLatin1String("\n\n");
    }
    return out;
}

bool TimelineModel::importSrt(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    m_subtitles = parseSrt(QString::fromUtf8(f.readAll()));
    f.close();
    emit subtitlesChanged();
    emit playheadChanged();   // recompón el PROGRAMA con el subtítulo activo
    return true;
}

bool TimelineModel::exportSrt(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    f.write(serializeSrt(m_subtitles).toUtf8());
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// Serialización del documento (proyecto .pvsproj)
// ---------------------------------------------------------------------------

namespace {

QJsonArray kfToJson(const QVector<TimelineModel::Keyframe> &kf)
{
    QJsonArray a;
    for (const auto &k : kf)
        a.append(QJsonObject{ { "t", double(k.sourceUs) }, { "v", k.value } });
    return a;
}

QVector<TimelineModel::Keyframe> kfFromJson(const QJsonValue &v)
{
    QVector<TimelineModel::Keyframe> out;
    for (const QJsonValue &e : v.toArray()) {
        const QJsonObject o = e.toObject();
        out.push_back({ qint64(o.value("t").toDouble()), o.value("v").toDouble() });
    }
    return out;
}

} // namespace

QJsonObject TimelineModel::toJson() const
{
    QJsonArray tracks;
    for (const Track &t : m_tracks)
        tracks.append(QJsonObject{
            { "name", t.name }, { "kind", t.kind }, { "idColor", t.idColor },
            { "height", t.height }, { "mute", t.mute }, { "solo", t.solo },
            { "gain", t.gain }, { "pan", t.pan },
            { "hidden", t.hidden }, { "locked", t.locked } });

    QJsonArray clips;
    for (const Clip &c : m_clips) {
        const Transform &tf = c.transform;
        const Color &co = c.color;
        QJsonObject o{
            { "id", double(c.id) }, { "track", c.trackIndex },
            { "name", c.name }, { "kind", c.kind },
            { "fill", c.fill }, { "border", c.border }, { "wav", c.wav },
            { "startUs", double(c.startUs) }, { "durationUs", double(c.durationUs) },
            { "inUs", double(c.inUs) }, { "mediaPath", c.mediaPath },
            { "speed", c.speed },
            { "transform", QJsonObject{
                { "posX", tf.posX }, { "posY", tf.posY }, { "scale", tf.scale },
                { "rotation", tf.rotation }, { "opacity", tf.opacity },
                { "cropL", tf.cropL }, { "cropT", tf.cropT },
                { "cropR", tf.cropR }, { "cropB", tf.cropB },
                { "kfPosX", kfToJson(tf.kfPosX) }, { "kfPosY", kfToJson(tf.kfPosY) },
                { "kfScale", kfToJson(tf.kfScale) }, { "kfRotation", kfToJson(tf.kfRotation) },
                { "kfOpacity", kfToJson(tf.kfOpacity) } } },
            { "color", QJsonObject{
                { "liftX", co.liftX }, { "liftY", co.liftY },
                { "gammaX", co.gammaX }, { "gammaY", co.gammaY },
                { "gainX", co.gainX }, { "gainY", co.gainY },
                { "temp", co.temp }, { "tint", co.tint }, { "sat", co.sat } } },
            { "audio", QJsonObject{
                { "gain", c.audio.gain }, { "pan", c.audio.pan }, { "mute", c.audio.mute },
                { "gainKf", kfToJson(c.audio.gainKf) }, { "panKf", kfToJson(c.audio.panKf) } } },
            { "title", QJsonObject{
                { "text", c.title.text }, { "sizePt", c.title.sizePt },
                { "color", c.title.color }, { "align", c.title.align },
                { "bar", c.title.bar }, { "barColor", c.title.barColor } } } };
        clips.append(o);
    }

    QJsonArray markers;
    for (const Marker &m : m_markers)
        markers.append(QJsonObject{ { "timeUs", double(m.timeUs) },
                                    { "color", m.color }, { "note", m.note } });

    QJsonArray subs;
    for (const Subtitle &s : m_subtitles)
        subs.append(QJsonObject{ { "startUs", double(s.startUs) },
                                 { "endUs", double(s.endUs) }, { "text", s.text } });

    return QJsonObject{
        { "totalUs", double(m_totalUs) }, { "playheadUs", double(m_playheadUs) },
        { "snap", m_snap }, { "subsEnabled", m_subsEnabled },
        { "tracks", tracks }, { "clips", clips },
        { "markers", markers }, { "subtitles", subs } };
}

bool TimelineModel::fromJson(const QJsonObject &o)
{
    if (!o.contains("tracks") || !o.contains("clips"))
        return false;

    QVector<Track> tracks;
    for (const QJsonValue &v : o.value("tracks").toArray()) {
        const QJsonObject t = v.toObject();
        Track tr;
        tr.name = t.value("name").toString();
        tr.kind = t.value("kind").toString();
        tr.idColor = t.value("idColor").toString();
        tr.height = t.value("height").toInt(48);
        tr.mute = t.value("mute").toBool();
        tr.solo = t.value("solo").toBool();
        tr.gain = t.value("gain").toDouble(1.0);
        tr.pan = t.value("pan").toDouble(0.0);
        tr.hidden = t.value("hidden").toBool();
        tr.locked = t.value("locked").toBool();
        tracks.push_back(tr);
    }
    if (tracks.isEmpty())
        return false;

    QVector<Clip> clips;
    quint64 maxId = 0;
    for (const QJsonValue &v : o.value("clips").toArray()) {
        const QJsonObject cj = v.toObject();
        Clip c;
        c.id = quint64(cj.value("id").toDouble());
        c.trackIndex = qBound(0, cj.value("track").toInt(), int(tracks.size()) - 1);
        c.name = cj.value("name").toString();
        c.kind = cj.value("kind").toString();
        c.fill = cj.value("fill").toString();
        c.border = cj.value("border").toString();
        c.wav = cj.value("wav").toString();
        c.startUs = qint64(cj.value("startUs").toDouble());
        c.durationUs = qint64(cj.value("durationUs").toDouble());
        c.inUs = qint64(cj.value("inUs").toDouble());
        c.mediaPath = cj.value("mediaPath").toString();
        c.speed = cj.value("speed").toDouble(1.0);
        const QJsonObject tf = cj.value("transform").toObject();
        c.transform.posX = tf.value("posX").toDouble();
        c.transform.posY = tf.value("posY").toDouble();
        c.transform.scale = tf.value("scale").toDouble(1.0);
        c.transform.rotation = tf.value("rotation").toDouble();
        c.transform.opacity = tf.value("opacity").toDouble(1.0);
        c.transform.cropL = tf.value("cropL").toDouble();
        c.transform.cropT = tf.value("cropT").toDouble();
        c.transform.cropR = tf.value("cropR").toDouble();
        c.transform.cropB = tf.value("cropB").toDouble();
        c.transform.kfPosX = kfFromJson(tf.value("kfPosX"));
        c.transform.kfPosY = kfFromJson(tf.value("kfPosY"));
        c.transform.kfScale = kfFromJson(tf.value("kfScale"));
        c.transform.kfRotation = kfFromJson(tf.value("kfRotation"));
        c.transform.kfOpacity = kfFromJson(tf.value("kfOpacity"));
        const QJsonObject co = cj.value("color").toObject();
        c.color.liftX = co.value("liftX").toDouble();
        c.color.liftY = co.value("liftY").toDouble();
        c.color.gammaX = co.value("gammaX").toDouble();
        c.color.gammaY = co.value("gammaY").toDouble();
        c.color.gainX = co.value("gainX").toDouble();
        c.color.gainY = co.value("gainY").toDouble();
        c.color.temp = co.value("temp").toDouble();
        c.color.tint = co.value("tint").toDouble();
        c.color.sat = co.value("sat").toDouble(1.0);
        const QJsonObject au = cj.value("audio").toObject();
        c.audio.gain = au.value("gain").toDouble(1.0);
        c.audio.pan = au.value("pan").toDouble(0.0);
        c.audio.mute = au.value("mute").toBool();
        c.audio.gainKf = kfFromJson(au.value("gainKf"));
        c.audio.panKf = kfFromJson(au.value("panKf"));
        const QJsonObject ti = cj.value("title").toObject();
        c.title.text = ti.value("text").toString();
        c.title.sizePt = ti.value("sizePt").toDouble(0.09);
        c.title.color = ti.value("color").toString(QStringLiteral("#ffffff"));
        c.title.align = ti.value("align").toInt(1);
        c.title.bar = ti.value("bar").toBool();
        c.title.barColor = ti.value("barColor").toString(QStringLiteral("#cc0e0f13"));
        maxId = qMax(maxId, c.id);
        clips.push_back(c);
    }

    QVector<Marker> markers;
    for (const QJsonValue &v : o.value("markers").toArray()) {
        const QJsonObject m = v.toObject();
        markers.push_back({ qint64(m.value("timeUs").toDouble()),
                            m.value("color").toString(), m.value("note").toString() });
    }
    QVector<Subtitle> subs;
    for (const QJsonValue &v : o.value("subtitles").toArray()) {
        const QJsonObject s = v.toObject();
        subs.push_back({ qint64(s.value("startUs").toDouble()),
                         qint64(s.value("endUs").toDouble()), s.value("text").toString() });
    }

    m_tracks = tracks;
    m_clips = clips;
    m_markers = markers;
    m_subtitles = subs;
    m_totalUs = qMax<qint64>(qint64(o.value("totalUs").toDouble()), 1000000);
    m_playheadUs = qBound<qint64>(0, qint64(o.value("playheadUs").toDouble()), m_totalUs);
    m_snap = o.value("snap").toBool(true);
    m_subsEnabled = o.value("subsEnabled").toBool(true);
    m_selectedId = 0;
    m_nextId = maxId + 1;
    m_undo.clear();

    emit changed();
    emit audioChanged();
    emit markersChanged();
    emit subtitlesChanged();
    emit selectionChanged();
    emit playheadChanged();
    emit snapChanged();
    return true;
}

void TimelineModel::openImportSrtDialog()
{
#ifdef Q_OS_WIN
    QVector<wchar_t> buf(4096, 0);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Subtítulos (*.srt)\0*.srt\0Todos los archivos\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = buf.size();
    ofn.lpstrTitle = L"Importar subtítulos";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
        importSrt(QString::fromWCharArray(buf.data()));
#endif
}

void TimelineModel::openExportSrtDialog()
{
#ifdef Q_OS_WIN
    QVector<wchar_t> buf(4096, 0);
    lstrcpynW(buf.data(), L"subtitulos.srt", buf.size());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Subtítulos (*.srt)\0*.srt\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = buf.size();
    ofn.lpstrTitle = L"Exportar subtítulos";
    ofn.lpstrDefExt = L"srt";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn))
        exportSrt(QString::fromWCharArray(buf.data()));
#endif
}

QVector<TimelineModel::AudioClip> TimelineModel::audioClips() const
{
    // Si alguna pista de audio está en solo, solo esas suenan.
    bool anySolo = false;
    for (const Track &t : m_tracks)
        if (t.solo && t.kind == QLatin1String("audio")) { anySolo = true; break; }

    QVector<AudioClip> out;
    for (const Clip &c : m_clips) {
        if (c.mediaPath.isEmpty() || c.kind == QLatin1String("title"))
            continue;
        const Track &tr = m_tracks.at(c.trackIndex);
        // Mute efectivo: mute de clip, o mute de pista, o (hay solo y esta pista no lo está).
        const bool eff = c.audio.mute || tr.mute || (anySolo && !tr.solo);
        // La ganancia/paneo de pista (fader/perilla del mezclador) se combinan con los
        // del clip: ganancia multiplicativa; paneo aditivo acotado a [-1, 1].
        const double gain = c.audio.gain * tr.gain;
        const double pan = qBound(-1.0, c.audio.pan + tr.pan, 1.0);
        QVector<Keyframe> gainKf = c.audio.gainKf;
        for (Keyframe &k : gainKf) k.value *= tr.gain;   // escala la automatización por el fader
        out.push_back({ c.mediaPath, c.trackIndex, c.startUs, c.durationUs, c.inUs,
                        c.speed, gain, pan, eff, gainKf, c.audio.panKf });
    }
    return out;
}

void TimelineModel::toggleKeyframe(const QString &prop)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    // Automatización de audio (listas propias en Clip::Audio).
    if (prop == QLatin1String("audioGain") || prop == QLatin1String("audioPan")) {
        Clip &c = m_clips[i];
        const bool isPan = prop == QLatin1String("audioPan");
        QVector<Keyframe> &kfa = isPan ? c.audio.panKf : c.audio.gainKf;
        const double sv = isPan ? c.audio.pan : c.audio.gain;
        const qint64 src = srcAtPlayhead(c);
        const qint64 tol = 20000;
        for (int k = 0; k < kfa.size(); ++k)
            if (qAbs(kfa[k].sourceUs - src) < tol) {
                kfa.remove(k); bumpSelection(); emit audioChanged(); return;
            }
        kfa.push_back({ src, evalKf(kfa, sv, src) });
        std::sort(kfa.begin(), kfa.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; });
        bumpSelection();
        emit audioChanged();
        return;
    }
    double *sv = nullptr;
    QVector<Keyframe> *kf = kfVec(m_clips[i].transform, prop, sv);
    if (!kf) return;
    const qint64 src = srcAtPlayhead(m_clips[i]);
    const qint64 tol = 20000;
    for (int k = 0; k < kf->size(); ++k) {
        if (qAbs(kf->at(k).sourceUs - src) < tol) { kf->remove(k); bumpSelection(); return; }
    }
    // Añade un keyframe con el valor evaluado actual (mantiene la apariencia en el playhead).
    kf->push_back({ src, evalKf(*kf, *sv, src) });
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; });
    bumpSelection();
}

bool TimelineModel::isKeyframed(const QString &prop) const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return false;
    if (prop == QLatin1String("audioGain"))
        return !m_clips[i].audio.gainKf.isEmpty();
    if (prop == QLatin1String("audioPan"))
        return !m_clips[i].audio.panKf.isEmpty();
    const QVector<Keyframe> *kf = kfVec(m_clips[i].transform, prop);
    return kf && !kf->isEmpty();
}

bool TimelineModel::hasKeyframeAtPlayhead(const QString &prop) const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return false;
    const QVector<Keyframe> *kf =
        prop == QLatin1String("audioGain") ? &m_clips[i].audio.gainKf
        : prop == QLatin1String("audioPan") ? &m_clips[i].audio.panKf
                                            : kfVec(m_clips[i].transform, prop);
    if (!kf) return false;
    const qint64 src = srcAtPlayhead(m_clips[i]);
    for (const Keyframe &k : *kf)
        if (qAbs(k.sourceUs - src) < 20000)
            return true;
    return false;
}

void TimelineModel::splitAtFraction(quint64 id, double timelineFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip orig = m_clips.at(idx);
    const qint64 splitUs = qint64(timelineFraction * m_totalUs);
    const qint64 offset = splitUs - orig.startUs;
    if (offset <= 0 || offset >= orig.durationUs)
        return; // el corte cae fuera del clip

    Clip right = orig;
    right.id = m_nextId++;
    right.startUs = splitUs;
    right.durationUs = orig.durationUs - offset;
    right.inUs = orig.inUs + qint64(offset * orig.speed); // avance de origen escalado por velocidad

    const qint64 leftDur = offset;
    const qint64 origDur = orig.durationUs;
    const quint64 leftId = orig.id;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Cortar clip"),
        [this, leftId, right, leftDur]() {
            const int i = indexOfClip(leftId);
            if (i < 0) return;
            m_clips[i].durationUs = leftDur;
            doInsert(right, i + 1);
            emit changed();
        },
        [this, leftId, origDur, rid = right.id]() {
            const int r = indexOfClip(rid);
            if (r >= 0) doRemoveAt(r);
            const int i = indexOfClip(leftId);
            if (i >= 0) m_clips[i].durationUs = origDur;
            emit changed();
        }));
}

void TimelineModel::splitSelectedAtPlayhead()
{
    const int idx = indexOfClip(m_selectedId);
    if (idx < 0)
        return;
    const Clip &c = m_clips.at(idx);
    if (m_playheadUs <= c.startUs || m_playheadUs >= c.startUs + c.durationUs)
        return; // el playhead no cae dentro del clip seleccionado
    splitAtFraction(m_selectedId, double(m_playheadUs) / m_totalUs);
}

void TimelineModel::removeSelected()
{
    const int idx = indexOfClip(m_selectedId);
    if (idx < 0)
        return;
    const Clip removed = m_clips.at(idx);

    m_undo.push(new TimelineCommand(
        QStringLiteral("Eliminar clip"),
        [this, rid = removed.id]() {
            const int i = indexOfClip(rid);
            if (i >= 0) doRemoveAt(i);
            m_selectedId = 0;
            emit changed();
        },
        [this, removed, idx]() {
            doInsert(removed, idx);
            emit changed();
        }));
}

void TimelineModel::moveClipToFraction(quint64 id, int trackIndex, double startFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const int oldTrack = m_clips[idx].trackIndex;
    const qint64 oldStart = m_clips[idx].startUs;
    const qint64 dur = m_clips[idx].durationUs;
    int newTrack = qBound(0, trackIndex, int(m_tracks.size()) - 1);
    // Solo se puede soltar en una pista del mismo tipo (audio↔audio, vídeo/título↔vídeo).
    const bool clipIsAudio = (m_clips[idx].kind == QLatin1String("audio"));
    if (newTrack != oldTrack && (m_tracks[newTrack].kind == QLatin1String("audio")) != clipIsAudio)
        newTrack = oldTrack;
    qint64 newStart = qMax<qint64>(0, qint64(startFraction * m_totalUs));

    // Imán: ajusta el borde (entrada o salida) que quede más cerca de un punto de anclaje.
    const qint64 snapStart = snapUs(newStart, id);
    const qint64 snapEnd = snapUs(newStart + dur, id) - dur;
    if (qAbs(snapStart - newStart) <= qAbs(snapEnd - newStart))
        newStart = snapStart;
    else
        newStart = snapEnd;
    newStart = qMax<qint64>(0, newStart);

    if (newTrack == oldTrack && newStart == oldStart)
        return;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Mover clip"),
        [this, id, newTrack, newStart]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].trackIndex = newTrack; m_clips[i].startUs = newStart; emit changed(); }
        },
        [this, id, oldTrack, oldStart]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].trackIndex = oldTrack; m_clips[i].startUs = oldStart; emit changed(); }
        }));
}

void TimelineModel::trimClip(quint64 id, bool leftEdge, double deltaFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip orig = m_clips.at(idx);
    const qint64 deltaUs = qint64(deltaFraction * m_totalUs);
    if (deltaUs == 0)
        return;

    qint64 newStart = orig.startUs;
    qint64 newDur = orig.durationUs;
    qint64 newInUs = orig.inUs;

    if (leftEdge) {
        // Mover el borde izquierdo: ajusta la entrada del origen y la posición.
        qint64 edge = snapUs(orig.startUs + deltaUs, id);
        edge = qBound<qint64>(qMax<qint64>(0, orig.startUs - orig.inUs),   // no antes del origen
                              edge,
                              orig.startUs + orig.durationUs - kMinClipUs); // deja duración mínima
        const qint64 applied = edge - orig.startUs;
        newStart = orig.startUs + applied;
        newDur = orig.durationUs - applied;
        newInUs = orig.inUs + qint64(applied * orig.speed);
    } else {
        // Mover el borde derecho: solo cambia la duración.
        qint64 edge = snapUs(orig.startUs + orig.durationUs + deltaUs, id);
        edge = qMax<qint64>(orig.startUs + kMinClipUs, edge);
        newDur = edge - orig.startUs;
    }

    if (newStart == orig.startUs && newDur == orig.durationUs)
        return;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Recortar clip"),
        [this, id, newStart, newDur, newInUs]() {
            const int i = indexOfClip(id);
            if (i < 0) return;
            m_clips[i].startUs = newStart;
            m_clips[i].durationUs = newDur;
            m_clips[i].inUs = newInUs;
            emit changed();
        },
        [this, id, os = orig.startUs, od = orig.durationUs, oi = orig.inUs]() {
            const int i = indexOfClip(id);
            if (i < 0) return;
            m_clips[i].startUs = os;
            m_clips[i].durationUs = od;
            m_clips[i].inUs = oi;
            emit changed();
        }));
}

void TimelineModel::slipClip(quint64 id, double deltaFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip orig = m_clips.at(idx);
    // Arrastrar a la derecha (delta>0) muestra contenido anterior → in disminuye.
    const qint64 deltaSrc = qint64(deltaFraction * m_totalUs * orig.speed);
    qint64 newIn = qMax<qint64>(0, orig.inUs - deltaSrc);
    if (newIn == orig.inUs)
        return;
    m_undo.push(new TimelineCommand(
        QStringLiteral("Deslizar clip"),
        [this, id, newIn]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].inUs = newIn; emit changed(); }
        },
        [this, id, oi = orig.inUs]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].inUs = oi; emit changed(); }
        }));
}

void TimelineModel::shiftTrack(int trackIndex, double deltaFraction)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    qint64 deltaUs = qint64(deltaFraction * m_totalUs);
    if (deltaUs == 0) return;

    // Acota el desplazamiento para que el clip más temprano de la pista no cruce el origen.
    qint64 minStart = std::numeric_limits<qint64>::max();
    bool any = false;
    for (const Clip &c : m_clips)
        if (c.trackIndex == trackIndex) { minStart = qMin(minStart, c.startUs); any = true; }
    if (!any) return;
    if (minStart + deltaUs < 0) deltaUs = -minStart;
    if (deltaUs == 0) return;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Desplazar pista"),
        [this, trackIndex, deltaUs]() {
            for (Clip &c : m_clips)
                if (c.trackIndex == trackIndex) c.startUs += deltaUs;
            emit changed();
        },
        [this, trackIndex, deltaUs]() {
            for (Clip &c : m_clips)
                if (c.trackIndex == trackIndex) c.startUs -= deltaUs;
            emit changed();
        }));
}

void TimelineModel::penToggleKeyframe(quint64 id, double timelineFraction, const QString &prop)
{
    if (indexOfClip(id) < 0) return;
    selectClip(id);
    setPlayheadFraction(qBound(0.0, timelineFraction, 1.0));
    toggleKeyframe(prop);
}

void TimelineModel::rippleTrimRight(quint64 id, double deltaFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip orig = m_clips.at(idx);
    const qint64 deltaUs = qint64(deltaFraction * m_totalUs);
    if (deltaUs == 0)
        return;

    qint64 edge = snapUs(orig.startUs + orig.durationUs + deltaUs, id);
    edge = qMax<qint64>(orig.startUs + kMinClipUs, edge);
    const qint64 newDur = edge - orig.startUs;
    const qint64 durChange = newDur - orig.durationUs;
    if (durChange == 0)
        return;

    // Clips posteriores en la misma pista: se desplazan para mantener el hueco.
    const int track = orig.trackIndex;
    const qint64 origEnd = orig.startUs + orig.durationUs;
    QVector<quint64> shiftIds;
    for (const Clip &c : m_clips)
        if (c.trackIndex == track && c.id != id && c.startUs >= origEnd)
            shiftIds.push_back(c.id);

    m_undo.push(new TimelineCommand(
        QStringLiteral("Ripple"),
        [this, id, newDur, durChange, shiftIds]() {
            const int i = indexOfClip(id);
            if (i >= 0) m_clips[i].durationUs = newDur;
            for (quint64 sid : shiftIds) { const int j = indexOfClip(sid); if (j >= 0) m_clips[j].startUs += durChange; }
            emit changed();
        },
        [this, id, od = orig.durationUs, durChange, shiftIds]() {
            const int i = indexOfClip(id);
            if (i >= 0) m_clips[i].durationUs = od;
            for (quint64 sid : shiftIds) { const int j = indexOfClip(sid); if (j >= 0) m_clips[j].startUs -= durChange; }
            emit changed();
        }));
}

void TimelineModel::rollEdit(quint64 id, bool leftEdge, double deltaFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip cur = m_clips.at(idx);
    const qint64 deltaUs = qint64(deltaFraction * m_totalUs);
    if (deltaUs == 0)
        return;

    const int track = cur.trackIndex;
    const qint64 boundary = leftEdge ? cur.startUs : (cur.startUs + cur.durationUs);
    const qint64 tol = qint64(m_totalUs * 0.002);

    // Busca el vecino que comparte la frontera.
    int nb = -1;
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].id == id || m_clips[i].trackIndex != track)
            continue;
        if (leftEdge) {
            if (qAbs((m_clips[i].startUs + m_clips[i].durationUs) - boundary) <= tol) { nb = i; break; }
        } else {
            if (qAbs(m_clips[i].startUs - boundary) <= tol) { nb = i; break; }
        }
    }
    if (nb < 0)
        return; // roll necesita un clip adyacente

    const Clip neighbor = m_clips.at(nb);
    qint64 target = snapUs(boundary + deltaUs, id);

    // Límites: ambos clips conservan duración mínima y no exponen fotogramas fuera del origen.
    qint64 lo, hi;
    if (!leftEdge) {
        // frontera = fin de cur = inicio de neighbor (siguiente)
        lo = qMax<qint64>(cur.startUs + kMinClipUs, boundary - neighbor.inUs);
        hi = (neighbor.startUs + neighbor.durationUs) - kMinClipUs;
    } else {
        // frontera = inicio de cur = fin de neighbor (anterior)
        lo = qMax<qint64>(neighbor.startUs + kMinClipUs, boundary - cur.inUs);
        hi = (cur.startUs + cur.durationUs) - kMinClipUs;
    }
    target = qBound(lo, target, hi);
    const qint64 delta = target - boundary;
    if (delta == 0)
        return;

    const quint64 curId = cur.id;
    const quint64 nbId = neighbor.id;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Roll"),
        [this, curId, nbId, leftEdge, delta]() {
            const int i = indexOfClip(curId);
            const int j = indexOfClip(nbId);
            if (i < 0 || j < 0) return;
            if (!leftEdge) {
                m_clips[i].durationUs += delta;                       // cur crece/mengua por la derecha
                m_clips[j].startUs += delta; m_clips[j].inUs += qint64(delta * m_clips[j].speed); m_clips[j].durationUs -= delta;
            } else {
                m_clips[j].durationUs += delta;                       // neighbor (anterior) mueve su salida
                m_clips[i].startUs += delta; m_clips[i].inUs += qint64(delta * m_clips[i].speed); m_clips[i].durationUs -= delta;
            }
            emit changed();
        },
        [this, curId, nbId, leftEdge, delta]() {
            const int i = indexOfClip(curId);
            const int j = indexOfClip(nbId);
            if (i < 0 || j < 0) return;
            if (!leftEdge) {
                m_clips[i].durationUs -= delta;
                m_clips[j].startUs -= delta; m_clips[j].inUs -= qint64(delta * m_clips[j].speed); m_clips[j].durationUs += delta;
            } else {
                m_clips[j].durationUs -= delta;
                m_clips[i].startUs -= delta; m_clips[i].inUs -= qint64(delta * m_clips[i].speed); m_clips[i].durationUs += delta;
            }
            emit changed();
        }));
}

qint64 TimelineModel::snapUs(qint64 us, quint64 excludeId) const
{
    if (!m_snap)
        return us;
    const qint64 tol = qint64(m_totalUs * 0.006); // ~1.8 s en la ventana de 5 min
    qint64 best = us;
    qint64 bestDist = tol + 1;
    auto consider = [&](qint64 edge) {
        const qint64 d = qAbs(edge - us);
        if (d < bestDist) { bestDist = d; best = edge; }
    };
    consider(0);
    consider(m_playheadUs);
    for (const Clip &c : m_clips) {
        if (c.id == excludeId)
            continue;
        consider(c.startUs);
        consider(c.startUs + c.durationUs);
    }
    for (const Marker &mk : m_markers)
        consider(mk.timeUs);
    return best;
}

void TimelineModel::setSnapEnabled(bool on)
{
    if (m_snap == on)
        return;
    m_snap = on;
    emit snapChanged();
}

void TimelineModel::undo()
{
    if (m_undo.canUndo()) { m_undo.undo(); emit changed(); }
}

void TimelineModel::redo()
{
    if (m_undo.canRedo()) { m_undo.redo(); emit changed(); }
}

void TimelineModel::setPlayheadFraction(double f)
{
    setPlayheadUs(qint64(f * m_totalUs));
}

void TimelineModel::setPlayheadUs(qint64 us)
{
    const qint64 v = qBound<qint64>(0, us, m_totalUs);
    if (v == m_playheadUs)
        return;
    m_playheadUs = v;
    emit playheadChanged();
}

qint64 TimelineModel::contentEndUs() const
{
    qint64 end = 0;
    for (const Clip &c : m_clips)
        end = qMax(end, c.startUs + c.durationUs);
    return end;
}

void TimelineModel::runSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_TL_SELFTEST"))
        return;

    // ---- Invariantes de edición: mover entre pistas, ripple y roll ----
    auto clipById = [this](quint64 id) -> Clip {
        const int i = indexOfClip(id);
        return i >= 0 ? m_clips[i] : Clip{};
    };
    auto v1sorted = [this]() {
        QVector<Clip> v;
        for (const Clip &c : m_clips) if (c.trackIndex == 2) v.push_back(c);
        std::sort(v.begin(), v.end(), [](const Clip &a, const Clip &b) { return a.startUs < b.startUs; });
        return v;
    };
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char *what) {
        if (ok) ++pass; else { ++fail; qWarning("[TL selftest] FALLO: %s", what); }
    };
    const bool snapWas = m_snap;
    setSnapEnabled(false); // resultados exactos, sin imán

    // 1) Mover a pista de vídeo compatible; rechazo hacia pista de audio.
    {
        const quint64 cid = v1sorted().first().id;
        const int t0 = clipById(cid).trackIndex;
        moveClipToFraction(cid, 1, 0.30);   // V2 (vídeo) → permitido
        check(clipById(cid).trackIndex == 1, "mover a pista de vídeo compatible");
        moveClipToFraction(cid, 3, 0.30);   // A1 (audio) → rechazado (misma pista/inicio, no-op)
        check(clipById(cid).trackIndex == 1, "rechazo de vídeo hacia pista de audio");
        undo();
        check(clipById(cid).trackIndex == t0, "undo restaura la pista original");
    }

    // 2) Ripple: recorta salida y desplaza los clips posteriores de la pista.
    {
        const auto v = v1sorted();
        const quint64 c0 = v[0].id, c1 = v[1].id;
        const qint64 d0 = clipById(c0).durationUs;
        const qint64 s1 = clipById(c1).startUs;
        const double df = 0.03;
        const qint64 ap = qint64(df * m_totalUs);
        rippleTrimRight(c0, df);
        check(clipById(c0).durationUs == d0 + ap, "ripple ajusta la duración");
        check(clipById(c1).startUs == s1 + ap, "ripple desplaza el clip posterior");
        undo();
        check(clipById(c0).durationUs == d0 && clipById(c1).startUs == s1, "undo de ripple restaura");
    }

    // 3) Roll: mueve la frontera entre dos clips adyacentes sin tocar el resto.
    {
        const auto v = v1sorted();
        const quint64 c0 = v[0].id, c1 = v[1].id, c2 = v[2].id;
        const qint64 c0dur = clipById(c0).durationUs;
        const qint64 c1start = clipById(c1).startUs, c1dur = clipById(c1).durationUs, c1in = clipById(c1).inUs;
        const qint64 c1end = c1start + c1dur;
        const qint64 c2start = clipById(c2).startUs;
        const double df = 0.02;
        const qint64 ap = qint64(df * m_totalUs);
        rollEdit(c0, false, df); // borde derecho de c0
        check(clipById(c0).durationUs == c0dur + ap, "roll extiende el clip actual");
        check(clipById(c1).startUs == c1start + ap && clipById(c1).inUs == c1in + ap, "roll mueve inicio/entrada del vecino");
        check(clipById(c1).startUs + clipById(c1).durationUs == c1end, "roll mantiene el fin del vecino");
        check(clipById(c2).startUs == c2start, "roll no mueve otros clips");
        undo();
        check(clipById(c0).durationUs == c0dur && clipById(c1).startUs == c1start, "undo de roll restaura");
    }

    // 4) clipsAt: resolución de capas de vídeo por pista, en orden abajo→arriba.
    {
        auto at = [this](double f) { return clipsAt(qint64(f * m_totalUs)); };
        const auto a = at(0.10);   // V1 (0.00–0.20) + V2 (0.06–0.22)
        check(a.size() == 2 && a[0].trackIndex == 2 && a[1].trackIndex == 1,
              "clipsAt(0.10): V1 abajo + V2 arriba");
        const auto b = at(0.30);   // V1 (0.20–0.46) + V3 título (0.24–0.44)
        check(b.size() == 2 && b[0].trackIndex == 2 && b[1].trackIndex == 0,
              "clipsAt(0.30): V1 abajo + V3 arriba");
        if (b.size() == 2)
            check(b[0].sourceUs == qint64(0.30 * m_totalUs) - qint64(0.20 * m_totalUs),
                  "clipsAt: sourceUs = inUs + (us - inicio)");
    }

    // 5) Keyframes: interpolación lineal de opacidad en el compositor.
    {
        const int ci = indexOfClip(v1sorted().first().id);
        const qint64 st = m_clips[ci].startUs;
        const qint64 dur = m_clips[ci].durationUs;
        m_clips[ci].transform.kfOpacity = { { 0, 0.0 }, { dur, 1.0 } };
        auto opAt = [this](qint64 us) {
            for (const auto &r : clipsAt(us)) if (r.trackIndex == 2) return r.transform.opacity;
            return -1.0;
        };
        setPlayheadUs(st + dur / 2);
        check(qAbs(opAt(playheadUs()) - 0.5) < 0.03, "keyframe: opacidad interpolada ~0.5");
        setPlayheadUs(st);
        check(opAt(playheadUs()) < 0.05, "keyframe: opacidad en el inicio ~0");
        m_clips[ci].transform.kfOpacity.clear();
        setPlayheadUs(0);
    }

    // 6) Transición: solape de dos clips de una pista → crossfade del entrante.
    {
        auto v = v1sorted();
        const int ia = indexOfClip(v[0].id);   // A (saliente)
        const int ib = indexOfClip(v[1].id);   // B (entrante)
        const qint64 aEnd = m_clips[ia].startUs + m_clips[ia].durationUs;
        const qint64 ov = qint64(0.04 * m_totalUs);
        const qint64 bStart0 = m_clips[ib].startUs;
        m_clips[ib].startUs = aEnd - ov;        // crea el solape [aEnd-ov, aEnd]
        const qint64 mid = aEnd - ov / 2;

        auto atMid = clipsAt(mid);
        int nT2 = 0; double bOp = -1;
        for (const auto &r : atMid) if (r.trackIndex == 2) { ++nT2; bOp = r.transform.opacity; }
        check(nT2 == 2, "transición: dos capas en la pista durante el solape");
        check(qAbs(bOp - 0.5) < 0.05, "transición: opacidad del entrante ~0.5 en el medio");

        m_clips[ib].startUs = bStart0;          // restaura
    }

    // 7) Remapeo de velocidad: sourceUs escala con speed.
    {
        const int ci = indexOfClip(v1sorted().first().id);
        const qint64 st = m_clips[ci].startUs, dur = m_clips[ci].durationUs;
        m_clips[ci].speed = 2.0;
        const qint64 off = dur / 4;
        setPlayheadUs(st + off);
        qint64 src = -1;
        for (const auto &r : clipsAt(playheadUs())) if (r.trackIndex == 2) src = r.sourceUs;
        check(src == qint64(off * 2.0), "velocidad 2x: sourceUs = 2·offset");
        m_clips[ci].speed = 1.0;
        setPlayheadUs(0);
    }

    // 8) Subtítulos: parseo SRT, round-trip y render inyectado en clipsAt.
    {
        const QString srt = QStringLiteral(
            "1\n00:00:01,000 --> 00:00:04,000\nHola mundo\n\n"
            "2\n00:00:05,500 --> 00:00:08,200\nSegunda línea\nmultilínea\n");
        QVector<Subtitle> subs = parseSrt(srt);
        check(subs.size() == 2, "srt: dos subtítulos parseados");
        check(subs.size() == 2 && subs[0].startUs == 1000000 && subs[0].endUs == 4000000,
              "srt: tiempos del primero");
        check(subs.size() == 2 && subs[1].text == QStringLiteral("Segunda línea\nmultilínea"),
              "srt: texto multilínea");
        QVector<Subtitle> rt = parseSrt(serializeSrt(subs));
        check(rt.size() == 2 && rt[1].startUs == subs[1].startUs && rt[1].text == subs[1].text,
              "srt: round-trip serialize→parse");
        m_subtitles = subs;
        setPlayheadUs(2000000);   // dentro del primer subtítulo
        bool found = false;
        for (const auto &r : clipsAt(playheadUs()))
            if (r.kind == QLatin1String("title") && r.title.text == QStringLiteral("Hola mundo"))
                found = true;
        check(found, "srt: subtítulo activo inyectado en el render");
        setPlayheadUs(4500000);   // hueco entre subtítulos
        bool none = true;
        for (const auto &r : clipsAt(playheadUs()))
            if (r.title.text == QStringLiteral("Hola mundo")) none = false;
        check(none, "srt: sin subtítulo fuera de intervalo");
        m_subtitles.clear();
        setPlayheadUs(0);
    }

    setSnapEnabled(snapWas);
    qWarning("[TL selftest] invariantes edición: %d OK, %d FALLO", pass, fail);

    const int n0 = clipCount();
    const quint64 firstV1 = [this]() -> quint64 {
        for (const Clip &c : m_clips) if (c.trackIndex == 2) return c.id;
        return 0;
    }();
    // corta el primer clip de V1 por su mitad
    const int idx = indexOfClip(firstV1);
    const double midFrac = double(m_clips[idx].startUs + m_clips[idx].durationUs / 2) / m_totalUs;
    splitAtFraction(firstV1, midFrac);
    const int n1 = clipCount();
    undo();
    const int n2 = clipCount();
    redo();
    const int n3 = clipCount();
    qWarning("[TL selftest] base=%d split=%d undo=%d redo=%d (esperado %d/%d/%d/%d)",
             n0, n1, n2, n3, n0, n0 + 1, n0, n0 + 1);
}
