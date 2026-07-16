#include "timelinemodel.h"

#include <QUndoCommand>
#include <QVariantMap>
#include <algorithm>
#include <functional>

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
        return { c.trackIndex, c.kind, c.fill, c.mediaPath, srcUs, rt, c.color };
    };

    // Pistas de vídeo de abajo (índice mayor, V1) hacia arriba (índice menor, V3),
    // para pintarlas en ese orden (las de arriba tapan a las de abajo).
    for (int t = m_tracks.size() - 1; t >= 0; --t) {
        if (m_tracks.at(t).kind != QLatin1String("video"))
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
    return out;
}

void TimelineModel::setClipMedia(quint64 id, const QString &path)
{
    const int i = indexOfClip(id);
    if (i < 0 || m_clips[i].mediaPath == path)
        return;
    m_clips[i].mediaPath = path;
    emit changed();
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
            m["selected"] = (c.id == m_selectedId);
            clips.append(m);
        }
        QVariantMap tm;
        tm["name"] = tr.name;
        tm["kind"] = tr.kind;
        tm["idColor"] = tr.idColor;
        tm["height"] = tr.height;
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
    emit selectionChanged();
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
    return i >= 0 ? m_clips[i].audio.pan : 0.0;
}
bool TimelineModel::selAudioMute() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].audio.mute;
}
void TimelineModel::setSelAudioGain(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(0.0, v, 4.0);
    Clip &c = m_clips[i];
    if (c.audio.gainKf.isEmpty()) {
        c.audio.gain = v;
    } else {
        const qint64 src = srcAtPlayhead(c);
        const qint64 tol = 20000;
        bool hit = false;
        for (int k = 0; k < c.audio.gainKf.size(); ++k)
            if (qAbs(c.audio.gainKf[k].sourceUs - src) < tol) { c.audio.gainKf[k].value = v; hit = true; break; }
        if (!hit) {
            c.audio.gainKf.push_back({ src, v });
            std::sort(c.audio.gainKf.begin(), c.audio.gainKf.end(),
                      [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; });
        }
    }
    bumpSelection();
    emit audioChanged();
}
void TimelineModel::setSelPan(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    v = qBound(-1.0, v, 1.0);
    if (m_clips[i].audio.pan == v) return;
    m_clips[i].audio.pan = v;
    bumpSelection();
    emit audioChanged();
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

QVector<TimelineModel::AudioClip> TimelineModel::audioClips() const
{
    QVector<AudioClip> out;
    for (const Clip &c : m_clips) {
        if (c.mediaPath.isEmpty() || c.kind == QLatin1String("title"))
            continue;
        out.push_back({ c.mediaPath, c.trackIndex, c.startUs, c.durationUs, c.inUs,
                        c.speed, c.audio.gain, c.audio.pan, c.audio.mute, c.audio.gainKf });
    }
    return out;
}

void TimelineModel::toggleKeyframe(const QString &prop)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    // Automatización de ganancia de audio (lista propia en Clip::Audio).
    if (prop == QLatin1String("audioGain")) {
        Clip &c = m_clips[i];
        const qint64 src = srcAtPlayhead(c);
        const qint64 tol = 20000;
        for (int k = 0; k < c.audio.gainKf.size(); ++k)
            if (qAbs(c.audio.gainKf[k].sourceUs - src) < tol) {
                c.audio.gainKf.remove(k); bumpSelection(); emit audioChanged(); return;
            }
        c.audio.gainKf.push_back({ src, evalKf(c.audio.gainKf, c.audio.gain, src) });
        std::sort(c.audio.gainKf.begin(), c.audio.gainKf.end(),
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
    const QVector<Keyframe> *kf = kfVec(m_clips[i].transform, prop);
    return kf && !kf->isEmpty();
}

bool TimelineModel::hasKeyframeAtPlayhead(const QString &prop) const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return false;
    const QVector<Keyframe> *kf =
        prop == QLatin1String("audioGain") ? &m_clips[i].audio.gainKf
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
