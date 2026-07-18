#include "timelinemodel.h"
#include "demodata.h"

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

    // La app arranca con la secuencia VACÍA. La data de demo solo se genera
    // para los autotests que dependen de un proyecto poblado, con PVS_DEMO=1
    // (que además usa los medios reales de LOCAL_DATA/) o con los hooks de
    // prueba PVS_TL_MEDIA / PVS_TL_AUDIO.
    const bool demoRequested = pvsEnvSet("PVS_DEMO")
        || pvsEnvSet("PVS_TL_SELFTEST") || pvsEnvSet("PVS_PROG_SELFTEST")
        || pvsEnvSet("PVS_PROJ_SELFTEST")
        || pvsEnvSet("PVS_TL_MEDIA") || pvsEnvSet("PVS_TL_AUDIO");
    if (!demoRequested)
        return;

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

    // PVS_DEMO=1: los clips de demo usan el primer vídeo de LOCAL_DATA/ —
    // vídeo en V1/V2 y su pista de audio en A1–A3 (si el archivo la trae).
    if (pvsEnvSet("PVS_DEMO")) {
        const QString localVideo = pvsFirstLocalVideo();
        if (!localVideo.isEmpty())
            for (Clip &c : m_clips)
                if (c.mediaPath.isEmpty() && c.kind != QLatin1String("title"))
                    c.mediaPath = localVideo;
    }
}

QVector<TimelineModel::RenderClip> TimelineModel::resolveClipsAt(
    const QVector<Track> &tracks, const QVector<Clip> &clips,
    const QVector<Subtitle> &subs, bool subsEnabled, qint64 us)
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
        Color rc = c.color;   // keyframes de color: se evalúan aquí (el worker no los ve)
        rc.temp = evalKf(c.color.kfTemp, c.color.temp, srcUs);
        rc.tint = evalKf(c.color.kfTint, c.color.tint, srcUs);
        rc.sat = evalKf(c.color.kfSat, c.color.sat, srcUs);
        rc.liftX = evalKf(c.color.kfLiftX, c.color.liftX, srcUs);
        rc.liftY = evalKf(c.color.kfLiftY, c.color.liftY, srcUs);
        rc.gammaX = evalKf(c.color.kfGammaX, c.color.gammaX, srcUs);
        rc.gammaY = evalKf(c.color.kfGammaY, c.color.gammaY, srcUs);
        rc.gainX = evalKf(c.color.kfGainX, c.color.gainX, srcUs);
        rc.gainY = evalKf(c.color.kfGainY, c.color.gainY, srcUs);
        rc.kfTemp.clear(); rc.kfTint.clear(); rc.kfSat.clear();
        rc.kfLiftX.clear(); rc.kfLiftY.clear(); rc.kfGammaX.clear();
        rc.kfGammaY.clear(); rc.kfGainX.clear(); rc.kfGainY.clear();
        // Bypass de nodos (Fusión): Transformar → geometría identidad (conserva la
        // opacidad, que es el nodo de salida); Color → corrección neutra.
        if (c.bypassTransform) {
            const double op = rt.opacity;
            rt = Transform{};
            rt.opacity = op;
        }
        if (c.bypassColor)
            rc = Color{};
        RenderClip r{ c.trackIndex, c.kind, c.fill, c.mediaPath, srcUs, rt, rc, c.title };
        r.clipId = c.id;
        return r;
    };

    // Pistas de vídeo de abajo (índice mayor, V1) hacia arriba (índice menor, V3),
    // para pintarlas en ese orden (las de arriba tapan a las de abajo).
    for (int t = tracks.size() - 1; t >= 0; --t) {
        if (tracks.at(t).kind != QLatin1String("video"))
            continue;
        if (tracks.at(t).hidden)   // pista de vídeo oculta (👁): no se compone
            continue;

        // Clips activos en la pista, ordenados por inicio.
        QVector<const Clip *> active;
        for (const Clip &c : clips)
            if (c.trackIndex == t && us >= c.startUs && us < c.startUs + c.durationUs)
                active.push_back(&c);
        if (active.isEmpty())
            continue;
        std::sort(active.begin(), active.end(),
                  [](const Clip *a, const Clip *b) { return a->startUs < b->startUs; });

        // Sin solape: un clip. Con solape: transición según el tipo del clip ENTRANTE
        // (B), con f = avance dentro de la región de solape:
        //  - "cross": el saliente (A) a plena opacidad y B fundiéndose encima;
        //  - "dip":   A se funde a negro en la primera mitad y B entra en la segunda;
        //  - "wipe":  B se revela de izquierda a derecha (clip-rect en el worker).
        const Clip *B = active.last();
        const Clip *A = active.size() >= 2 ? active.at(active.size() - 2) : nullptr;
        double f = 1.0;
        if (A) {
            const qint64 ovStart = B->startUs;                    // inicio del solape
            const qint64 ovEnd = A->startUs + A->durationUs;      // fin del solape (fin de A)
            if (ovEnd > ovStart)
                f = qBound(0.0, double(us - ovStart) / double(ovEnd - ovStart), 1.0);
        }
        if (A && B->transition == QLatin1String("dip")) {
            out.push_back(resolved(*A, us, qMax(0.0, 1.0 - f * 2)));   // 1→0 en la 1.ª mitad
            out.push_back(resolved(*B, us, qMax(0.0, f * 2 - 1.0)));   // 0→1 en la 2.ª mitad
        } else if (A && B->transition == QLatin1String("wipe")) {
            out.push_back(resolved(*A, us, 1.0));
            RenderClip rb = resolved(*B, us, 1.0);
            rb.wipe = f;                                               // revelado por barrido
            out.push_back(rb);
        } else {
            if (A)
                out.push_back(resolved(*A, us, 1.0));                  // saliente debajo
            out.push_back(resolved(*B, us, f));                        // entrante (crossfade)
        }
    }

    // Subtítulo activo: se pinta encima de todo como un título en el tercio inferior.
    if (subsEnabled) {
        for (const Subtitle &s : subs) {
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

QVector<TimelineModel::RenderClip> TimelineModel::clipsAt(qint64 us) const
{
    return resolveClipsAt(m_tracks, m_clips, m_subtitles, m_subsEnabled, us);
}

TimelineModel::RenderSnapshot TimelineModel::renderSnapshot() const
{
    return RenderSnapshot{ m_tracks, m_clips, m_subtitles, m_subsEnabled };
}

QVector<TimelineModel::RenderClip> TimelineModel::clipsAtSnapshot(const RenderSnapshot &snap, qint64 us)
{
    return resolveClipsAt(snap.tracks, snap.clips, snap.subtitles, snap.subsEnabled, us);
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

void TimelineModel::growWindow(qint64 neededUs)
{
    if (neededUs <= m_totalUs)
        return;
    // Redondea hacia arriba al minuto siguiente y añade 1 min de margen.
    const qint64 minute = 60LL * 1000000;
    const qint64 target = ((neededUs + minute - 1) / minute + 1) * minute;
    m_totalUs = qMax(m_totalUs, target);
    emit changed();
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
            m["transition"] = c.transition;
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

QAbstractItemModel *TimelineModel::tracksModel()
{
    if (!m_tracksModel)
        m_tracksModel = new TimelineTracksModel(this, false, this);
    return m_tracksModel;
}

QAbstractItemModel *TimelineModel::audioTracksModel()
{
    if (!m_audioTracksModel)
        m_audioTracksModel = new TimelineTracksModel(this, true, this);
    return m_audioTracksModel;
}

// ==================== TimelineTracksModel (adaptador QAIM) ====================

TimelineTracksModel::TimelineTracksModel(TimelineModel *tm, bool audio, QObject *parent)
    : QAbstractListModel(parent), m_tm(tm), m_audio(audio)
{
    m_rows = m_audio ? m_tm->audioTracks() : m_tm->tracks();
    // Mismas señales de las que dependían las propiedades QVariantList equivalentes.
    if (m_audio) {
        connect(m_tm, &TimelineModel::audioChanged, this, &TimelineTracksModel::refresh);
    } else {
        connect(m_tm, &TimelineModel::changed, this, &TimelineTracksModel::refresh);
    }
}

QVariant TimelineTracksModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    if (role == TrackDataRole || role == Qt::DisplayRole)
        return m_rows.at(index.row());
    return {};
}

void TimelineTracksModel::refresh()
{
    const QVariantList rows = m_audio ? m_tm->audioTracks() : m_tm->tracks();
    if (rows.size() != m_rows.size()) {
        beginResetModel();
        m_rows = rows;
        endResetModel();
        return;
    }
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i) != m_rows.at(i)) {
            m_rows[i] = rows.at(i);
            const QModelIndex idx = index(i, 0);
            emit dataChanged(idx, idx, { TrackDataRole });
        }
    }
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
            if (a.interp == 1)
                return a.value;                    // hold: mantiene hasta el siguiente
            const double span = double(b.sourceUs - a.sourceUs);
            double t = span > 0 ? double(sourceUs - a.sourceUs) / span : 0.0;
            if (a.interp == 2)
                t = t * t * (3.0 - 2.0 * t);       // suave: smoothstep (ease in-out)
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

// ---- Corrección de color del clip seleccionado (evaluada en el playhead) ----
double TimelineModel::selLiftX() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfLiftX, m_clips[i].color.liftX, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selLiftY() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfLiftY, m_clips[i].color.liftY, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selGammaX() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfGammaX, m_clips[i].color.gammaX, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selGammaY() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfGammaY, m_clips[i].color.gammaY, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selGainX() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfGainX, m_clips[i].color.gainX, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selGainY() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfGainY, m_clips[i].color.gainY, srcAtPlayhead(m_clips[i])) : 0.0; }
// temp/tint/sat son animables: se evalúan en el playhead (como la transformación).
double TimelineModel::selTemp() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfTemp, m_clips[i].color.temp, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selTint() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfTint, m_clips[i].color.tint, srcAtPlayhead(m_clips[i])) : 0.0; }
double TimelineModel::selSat() const { const int i = indexOfClip(m_selectedId); return i >= 0 ? evalKf(m_clips[i].color.kfSat, m_clips[i].color.sat, srcAtPlayhead(m_clips[i])) : 1.0; }

// Aplica un valor a una propiedad de color animable: al keyframe del playhead si
// está animada (actualiza el cercano o inserta), o al valor estático si no.
static void applyColorKf(QVector<TimelineModel::Keyframe> &kf, double &staticVal, qint64 src, double v)
{
    if (kf.isEmpty()) { staticVal = v; return; }
    const qint64 tol = 20000;
    for (int i = 0; i < kf.size(); ++i)
        if (qAbs(kf[i].sourceUs - src) < tol) { kf[i].value = v; return; }
    kf.push_back({ src, v });
    std::sort(kf.begin(), kf.end(),
              [](const TimelineModel::Keyframe &a, const TimelineModel::Keyframe &b) { return a.sourceUs < b.sourceUs; });
}

void TimelineModel::setSelLift(double x, double y)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Clip &c = m_clips[i];
    applyColorKf(c.color.kfLiftX, c.color.liftX, srcAtPlayhead(c), qBound(-1.0, x, 1.0));
    applyColorKf(c.color.kfLiftY, c.color.liftY, srcAtPlayhead(c), qBound(-1.0, y, 1.0));
    bumpSelection();
}
void TimelineModel::setSelGamma(double x, double y)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Clip &c = m_clips[i];
    applyColorKf(c.color.kfGammaX, c.color.gammaX, srcAtPlayhead(c), qBound(-1.0, x, 1.0));
    applyColorKf(c.color.kfGammaY, c.color.gammaY, srcAtPlayhead(c), qBound(-1.0, y, 1.0));
    bumpSelection();
}
void TimelineModel::setSelGain(double x, double y)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Clip &c = m_clips[i];
    applyColorKf(c.color.kfGainX, c.color.gainX, srcAtPlayhead(c), qBound(-1.0, x, 1.0));
    applyColorKf(c.color.kfGainY, c.color.gainY, srcAtPlayhead(c), qBound(-1.0, y, 1.0));
    bumpSelection();
}
void TimelineModel::setSelTemp(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Clip &c = m_clips[i];
    applyColorKf(c.color.kfTemp, c.color.temp, srcAtPlayhead(c), qBound(-1.0, v, 1.0));
    bumpSelection();
}
void TimelineModel::setSelTint(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Clip &c = m_clips[i];
    applyColorKf(c.color.kfTint, c.color.tint, srcAtPlayhead(c), qBound(-1.0, v, 1.0));
    bumpSelection();
}
void TimelineModel::setSelSat(double v)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Clip &c = m_clips[i];
    applyColorKf(c.color.kfSat, c.color.sat, srcAtPlayhead(c), qBound(0.0, v, 2.0));
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

void TimelineModel::setMasterGain(double gain)
{
    gain = qBound(0.0, gain, 4.0);   // hasta +12 dB
    if (qFuzzyCompare(m_masterGain, gain)) return;
    m_masterGain = gain;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setMasterPan(double pan)
{
    pan = qBound(-1.0, pan, 1.0);
    if (qFuzzyCompare(m_masterPan, pan)) return;
    m_masterPan = pan;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setMasterLimiterOn(bool on)
{
    if (m_masterLimiterOn == on) return;
    m_masterLimiterOn = on;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setMasterCeilingDb(double db)
{
    db = qBound(-12.0, db, 0.0);
    if (qFuzzyCompare(m_masterCeilingDb, db)) return;
    m_masterCeilingDb = db;
    emit audioChanged();
    emit edited();
}

// ---- Efectos de audio por pista ----
void TimelineModel::setTrackEqEnabled(int trackIndex, bool on)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks[trackIndex].eqOn == on) return;
    m_tracks[trackIndex].eqOn = on;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setTrackEq(int trackIndex, double lowDb, double midDb, double highDb)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    lowDb = qBound(-18.0, lowDb, 18.0);
    midDb = qBound(-18.0, midDb, 18.0);
    highDb = qBound(-18.0, highDb, 18.0);
    Track &t = m_tracks[trackIndex];
    if (qFuzzyCompare(t.eqLowDb, lowDb) && qFuzzyCompare(t.eqMidDb, midDb)
        && qFuzzyCompare(t.eqHighDb, highDb))
        return;
    t.eqLowDb = lowDb; t.eqMidDb = midDb; t.eqHighDb = highDb;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setTrackCompEnabled(int trackIndex, bool on)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks[trackIndex].compOn == on) return;
    m_tracks[trackIndex].compOn = on;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setTrackComp(int trackIndex, double threshDb, double ratio, double makeupDb)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    threshDb = qBound(-48.0, threshDb, 0.0);
    ratio = qBound(1.0, ratio, 20.0);
    makeupDb = qBound(0.0, makeupDb, 24.0);
    Track &t = m_tracks[trackIndex];
    if (qFuzzyCompare(t.compThreshDb, threshDb) && qFuzzyCompare(t.compRatio, ratio)
        && qFuzzyCompare(t.compMakeupDb, makeupDb))
        return;
    t.compThreshDb = threshDb; t.compRatio = ratio; t.compMakeupDb = makeupDb;
    emit audioChanged();
    emit edited();
}
void TimelineModel::setTrackGateEnabled(int trackIndex, bool on)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size() || m_tracks[trackIndex].gateOn == on) return;
    m_tracks[trackIndex].gateOn = on;
    emit audioChanged(); emit edited();
}
void TimelineModel::setTrackGate(int trackIndex, double threshDb)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    threshDb = qBound(-80.0, threshDb, 0.0);
    if (qFuzzyCompare(m_tracks[trackIndex].gateThreshDb, threshDb)) return;
    m_tracks[trackIndex].gateThreshDb = threshDb;
    emit audioChanged(); emit edited();
}
void TimelineModel::setTrackDeEsserEnabled(int trackIndex, bool on)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size() || m_tracks[trackIndex].deEssOn == on) return;
    m_tracks[trackIndex].deEssOn = on;
    emit audioChanged(); emit edited();
}
void TimelineModel::setTrackDeEsser(int trackIndex, double threshDb)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    threshDb = qBound(-48.0, threshDb, 0.0);
    if (qFuzzyCompare(m_tracks[trackIndex].deEssThreshDb, threshDb)) return;
    m_tracks[trackIndex].deEssThreshDb = threshDb;
    emit audioChanged(); emit edited();
}
void TimelineModel::setTrackReverbEnabled(int trackIndex, bool on)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size() || m_tracks[trackIndex].reverbOn == on) return;
    m_tracks[trackIndex].reverbOn = on;
    emit audioChanged(); emit edited();
}
void TimelineModel::setTrackReverb(int trackIndex, double mix, double size)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    mix = qBound(0.0, mix, 1.0);
    size = qBound(0.0, size, 1.0);
    Track &t = m_tracks[trackIndex];
    if (qFuzzyCompare(t.reverbMix, mix) && qFuzzyCompare(t.reverbSize, size)) return;
    t.reverbMix = mix; t.reverbSize = size;
    emit audioChanged(); emit edited();
}

// ==================== Gestión de pistas ====================

void TimelineModel::insertTrackAt(const Track &t, int at)
{
    at = qBound(0, at, int(m_tracks.size()));
    for (Clip &c : m_clips)
        if (c.trackIndex >= at)
            c.trackIndex += 1;
    m_tracks.insert(at, t);
}

void TimelineModel::removeTrackAt(int at)
{
    if (at < 0 || at >= m_tracks.size()) return;
    for (int i = int(m_clips.size()) - 1; i >= 0; --i) {
        if (m_clips[i].trackIndex == at) {
            if (m_clips[i].id == m_selectedId) m_selectedId = 0;
            m_clips.remove(i);
        } else if (m_clips[i].trackIndex > at) {
            m_clips[i].trackIndex -= 1;
        }
    }
    m_tracks.remove(at);
}

QString TimelineModel::uniqueTrackName(const QString &kind) const
{
    const QChar prefix = (kind == QLatin1String("audio")) ? QLatin1Char('A') : QLatin1Char('V');
    int maxN = 0;
    for (const Track &t : m_tracks) {
        if (t.kind != kind || t.name.isEmpty() || t.name.at(0) != prefix)
            continue;
        bool ok = false;
        const int n = t.name.mid(1).toInt(&ok);
        if (ok && n > maxN) maxN = n;
    }
    return QString(prefix) + QString::number(maxN + 1);
}

void TimelineModel::addVideoTrack()
{
    Track t;
    t.name = uniqueTrackName(QStringLiteral("video"));
    t.kind = QStringLiteral("video");
    t.idColor = QStringLiteral("#5b8dd6");
    t.height = 56;
    // El vídeo se añade ARRIBA (índice 0): la pista nueva queda como la capa superior.
    m_undo.push(new TimelineCommand(
        QStringLiteral("Añadir pista de vídeo"),
        [this, t]() { insertTrackAt(t, 0); emit changed(); emit audioChanged(); bumpSelection(); },
        [this]() { removeTrackAt(0); emit changed(); emit audioChanged(); bumpSelection(); }));
}

void TimelineModel::addAudioTrack()
{
    Track t;
    t.name = uniqueTrackName(QStringLiteral("audio"));
    t.kind = QStringLiteral("audio");
    t.idColor = QStringLiteral("#4a9e6b");
    t.height = 48;
    // El audio se añade al FINAL (mantiene el invariante vídeo-antes-que-audio).
    const int at = int(m_tracks.size());
    m_undo.push(new TimelineCommand(
        QStringLiteral("Añadir pista de audio"),
        [this, t, at]() { insertTrackAt(t, at); emit changed(); emit audioChanged(); },
        [this, at]() { removeTrackAt(at); emit changed(); emit audioChanged(); }));
}

void TimelineModel::removeTrack(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    if (m_tracks.size() <= 1) return;   // no dejar la secuencia sin ninguna pista
    const Track removedTrack = m_tracks.at(trackIndex);
    // Instantánea de los clips de esta pista para el undo (vuelven con su trackIndex).
    QVector<Clip> removedClips;
    for (const Clip &c : m_clips)
        if (c.trackIndex == trackIndex)
            removedClips.push_back(c);

    m_undo.push(new TimelineCommand(
        QStringLiteral("Eliminar pista"),
        [this, trackIndex]() { removeTrackAt(trackIndex); emit changed(); emit audioChanged(); bumpSelection(); },
        [this, removedTrack, trackIndex, removedClips]() {
            insertTrackAt(removedTrack, trackIndex);   // reindexa +1 los clips >= trackIndex
            for (const Clip &c : removedClips)         // los eliminados vuelven a su pista
                m_clips.push_back(c);
            emit changed(); emit audioChanged(); bumpSelection();
        }));
}

void TimelineModel::renameTrack(int trackIndex, const QString &name)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size()) return;
    const QString n = name.trimmed();
    if (n.isEmpty() || n == m_tracks[trackIndex].name) return;
    const QString old = m_tracks[trackIndex].name;
    m_undo.push(new TimelineCommand(
        QStringLiteral("Renombrar pista"),
        [this, trackIndex, n]() {
            if (trackIndex < m_tracks.size()) { m_tracks[trackIndex].name = n; emit changed(); emit audioChanged(); }
        },
        [this, trackIndex, old]() {
            if (trackIndex < m_tracks.size()) { m_tracks[trackIndex].name = old; emit changed(); emit audioChanged(); }
        }));
}

QVariantList TimelineModel::audioTracks() const
{
    QVariantList out;
    for (int t = 0; t < m_tracks.size(); ++t) {
        const Track &tr = m_tracks[t];
        out.append(QVariantMap{
            { "index", t }, { "name", tr.name }, { "kind", tr.kind },
            { "mute", tr.mute }, { "solo", tr.solo },
            { "gain", tr.gain }, { "pan", tr.pan },
            { "eqOn", tr.eqOn }, { "eqLowDb", tr.eqLowDb },
            { "eqMidDb", tr.eqMidDb }, { "eqHighDb", tr.eqHighDb },
            { "compOn", tr.compOn }, { "compThreshDb", tr.compThreshDb },
            { "compRatio", tr.compRatio }, { "compMakeupDb", tr.compMakeupDb },
            { "gateOn", tr.gateOn }, { "gateThreshDb", tr.gateThreshDb },
            { "deEssOn", tr.deEssOn }, { "deEssThreshDb", tr.deEssThreshDb },
            { "reverbOn", tr.reverbOn }, { "reverbMix", tr.reverbMix }, { "reverbSize", tr.reverbSize } });
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

// ---- Efectos de audio por clip (clip seleccionado) ----
bool TimelineModel::selAudioEqOn() const
{ const int i = indexOfClip(m_selectedId); return i >= 0 && m_clips[i].audio.eqOn; }
double TimelineModel::selAudioEqLowDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return 0.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.eqLowKf, c.audio.eqLowDb, srcAtPlayhead(c)); }
double TimelineModel::selAudioEqMidDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return 0.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.eqMidKf, c.audio.eqMidDb, srcAtPlayhead(c)); }
double TimelineModel::selAudioEqHighDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return 0.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.eqHighKf, c.audio.eqHighDb, srcAtPlayhead(c)); }
bool TimelineModel::selAudioCompOn() const
{ const int i = indexOfClip(m_selectedId); return i >= 0 && m_clips[i].audio.compOn; }
double TimelineModel::selAudioCompThreshDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return -18.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.compThreshKf, c.audio.compThreshDb, srcAtPlayhead(c)); }
double TimelineModel::selAudioCompRatio() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return 2.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.compRatioKf, c.audio.compRatio, srcAtPlayhead(c)); }
double TimelineModel::selAudioCompMakeupDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return 0.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.compMakeupKf, c.audio.compMakeupDb, srcAtPlayhead(c)); }

void TimelineModel::setSelAudioEqEnabled(bool on)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].audio.eqOn == on) return;
    m_clips[i].audio.eqOn = on;
    bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioEq(double lowDb, double midDb, double highDb)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Audio &a = m_clips[i].audio;
    const qint64 src = srcAtPlayhead(m_clips[i]);   // escribe al keyframe del playhead si está animado
    applyColorKf(a.eqLowKf,  a.eqLowDb,  src, qBound(-18.0, lowDb, 18.0));
    applyColorKf(a.eqMidKf,  a.eqMidDb,  src, qBound(-18.0, midDb, 18.0));
    applyColorKf(a.eqHighKf, a.eqHighDb, src, qBound(-18.0, highDb, 18.0));
    bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioCompEnabled(bool on)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].audio.compOn == on) return;
    m_clips[i].audio.compOn = on;
    bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioComp(double threshDb, double ratio, double makeupDb)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Audio &a = m_clips[i].audio;
    const qint64 src = srcAtPlayhead(m_clips[i]);
    applyColorKf(a.compThreshKf, a.compThreshDb, src, qBound(-48.0, threshDb, 0.0));
    applyColorKf(a.compRatioKf,  a.compRatio,    src, qBound(1.0, ratio, 20.0));
    applyColorKf(a.compMakeupKf, a.compMakeupDb, src, qBound(0.0, makeupDb, 24.0));
    bumpSelection(); emit audioChanged();
}

// Procesadores adicionales por clip (puerta · de-esser · reverb).
bool TimelineModel::selAudioGateOn() const
{ const int i = indexOfClip(m_selectedId); return i >= 0 && m_clips[i].audio.gateOn; }
double TimelineModel::selAudioGateThreshDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return -40.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.gateThreshKf, c.audio.gateThreshDb, srcAtPlayhead(c)); }
bool TimelineModel::selAudioDeEssOn() const
{ const int i = indexOfClip(m_selectedId); return i >= 0 && m_clips[i].audio.deEssOn; }
double TimelineModel::selAudioDeEssThreshDb() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return -24.0;
  const Clip &c = m_clips[i]; return evalKf(c.audio.deEssThreshKf, c.audio.deEssThreshDb, srcAtPlayhead(c)); }
bool TimelineModel::selAudioReverbOn() const
{ const int i = indexOfClip(m_selectedId); return i >= 0 && m_clips[i].audio.reverbOn; }
double TimelineModel::selAudioReverbMix() const
{ const int i = indexOfClip(m_selectedId); if (i < 0) return 0.25;
  const Clip &c = m_clips[i]; return evalKf(c.audio.reverbMixKf, c.audio.reverbMix, srcAtPlayhead(c)); }
double TimelineModel::selAudioReverbSize() const
{ const int i = indexOfClip(m_selectedId); return i >= 0 ? m_clips[i].audio.reverbSize : 0.5; }

void TimelineModel::setSelAudioGateEnabled(bool on)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].audio.gateOn == on) return;
    m_clips[i].audio.gateOn = on; bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioGate(double threshDb)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    applyColorKf(m_clips[i].audio.gateThreshKf, m_clips[i].audio.gateThreshDb,
                 srcAtPlayhead(m_clips[i]), qBound(-80.0, threshDb, 0.0));
    bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioDeEsserEnabled(bool on)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].audio.deEssOn == on) return;
    m_clips[i].audio.deEssOn = on; bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioDeEsser(double threshDb)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    applyColorKf(m_clips[i].audio.deEssThreshKf, m_clips[i].audio.deEssThreshDb,
                 srcAtPlayhead(m_clips[i]), qBound(-48.0, threshDb, 0.0));
    bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioReverbEnabled(bool on)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].audio.reverbOn == on) return;
    m_clips[i].audio.reverbOn = on; bumpSelection(); emit audioChanged();
}
void TimelineModel::setSelAudioReverb(double mix, double size)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    Audio &a = m_clips[i].audio;
    applyColorKf(a.reverbMixKf, a.reverbMix, srcAtPlayhead(m_clips[i]), qBound(0.0, mix, 1.0));
    a.reverbSize = qBound(0.0, size, 1.0);   // el tamaño (líneas de retardo) no se automatiza
    bumpSelection(); emit audioChanged();
}

bool TimelineModel::selIsAudio() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].kind == QLatin1String("audio");
}

// ---- Bypass de nodos (página Fusión) ----
bool TimelineModel::selBypassTransform() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].bypassTransform;
}
bool TimelineModel::selBypassColor() const
{
    const int i = indexOfClip(m_selectedId);
    return i >= 0 && m_clips[i].bypassColor;
}
void TimelineModel::setSelBypassTransform(bool bypass)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].bypassTransform == bypass) return;
    m_clips[i].bypassTransform = bypass;
    bumpSelection();
    emit changed();   // recompón el PROGRAMA
    emit edited();
}
void TimelineModel::setSelBypassColor(bool bypass)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0 || m_clips[i].bypassColor == bypass) return;
    m_clips[i].bypassColor = bypass;
    bumpSelection();
    emit changed();
    emit edited();
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
    qint64 start = qMax<qint64>(0, qint64(startFraction * m_totalUs));
    // No truncar el medio: crece la ventana para que el clip completo quepa.
    growWindow(start + dur);
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
    for (const auto &k : kf) {
        QJsonObject o{ { "t", double(k.sourceUs) }, { "v", k.value } };
        if (k.interp != 0)
            o.insert("i", k.interp);   // interpolación (0 lineal se omite)
        a.append(o);
    }
    return a;
}

QVector<TimelineModel::Keyframe> kfFromJson(const QJsonValue &v)
{
    QVector<TimelineModel::Keyframe> out;
    for (const QJsonValue &e : v.toArray()) {
        const QJsonObject o = e.toObject();
        out.push_back({ qint64(o.value("t").toDouble()), o.value("v").toDouble(),
                        o.value("i").toInt(0) });
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
            { "hidden", t.hidden }, { "locked", t.locked },
            { "eqOn", t.eqOn }, { "eqLowDb", t.eqLowDb },
            { "eqMidDb", t.eqMidDb }, { "eqHighDb", t.eqHighDb },
            { "compOn", t.compOn }, { "compThreshDb", t.compThreshDb },
            { "compRatio", t.compRatio }, { "compMakeupDb", t.compMakeupDb },
            { "gateOn", t.gateOn }, { "gateThreshDb", t.gateThreshDb },
            { "deEssOn", t.deEssOn }, { "deEssThreshDb", t.deEssThreshDb },
            { "reverbOn", t.reverbOn }, { "reverbMix", t.reverbMix }, { "reverbSize", t.reverbSize } });

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
            { "speed", c.speed }, { "transition", c.transition },
            { "bypassTransform", c.bypassTransform }, { "bypassColor", c.bypassColor },
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
                { "temp", co.temp }, { "tint", co.tint }, { "sat", co.sat },
                { "kfTemp", kfToJson(co.kfTemp) }, { "kfTint", kfToJson(co.kfTint) },
                { "kfSat", kfToJson(co.kfSat) },
                { "kfLiftX", kfToJson(co.kfLiftX) }, { "kfLiftY", kfToJson(co.kfLiftY) },
                { "kfGammaX", kfToJson(co.kfGammaX) }, { "kfGammaY", kfToJson(co.kfGammaY) },
                { "kfGainX", kfToJson(co.kfGainX) }, { "kfGainY", kfToJson(co.kfGainY) } } },
            { "audio", QJsonObject{
                { "gain", c.audio.gain }, { "pan", c.audio.pan }, { "mute", c.audio.mute },
                { "gainKf", kfToJson(c.audio.gainKf) }, { "panKf", kfToJson(c.audio.panKf) },
                { "eqOn", c.audio.eqOn }, { "eqLowDb", c.audio.eqLowDb },
                { "eqMidDb", c.audio.eqMidDb }, { "eqHighDb", c.audio.eqHighDb },
                { "compOn", c.audio.compOn }, { "compThreshDb", c.audio.compThreshDb },
                { "compRatio", c.audio.compRatio }, { "compMakeupDb", c.audio.compMakeupDb },
                { "gateOn", c.audio.gateOn }, { "gateThreshDb", c.audio.gateThreshDb },
                { "deEssOn", c.audio.deEssOn }, { "deEssThreshDb", c.audio.deEssThreshDb },
                { "reverbOn", c.audio.reverbOn }, { "reverbMix", c.audio.reverbMix },
                { "reverbSize", c.audio.reverbSize },
                { "eqLowKf", kfToJson(c.audio.eqLowKf) }, { "eqMidKf", kfToJson(c.audio.eqMidKf) },
                { "eqHighKf", kfToJson(c.audio.eqHighKf) }, { "reverbMixKf", kfToJson(c.audio.reverbMixKf) },
                { "compThreshKf", kfToJson(c.audio.compThreshKf) }, { "compRatioKf", kfToJson(c.audio.compRatioKf) },
                { "compMakeupKf", kfToJson(c.audio.compMakeupKf) }, { "gateThreshKf", kfToJson(c.audio.gateThreshKf) },
                { "deEssThreshKf", kfToJson(c.audio.deEssThreshKf) } } },
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
        { "masterGain", m_masterGain }, { "masterPan", m_masterPan },
        { "masterLimiterOn", m_masterLimiterOn }, { "masterCeilingDb", m_masterCeilingDb },
        { "markInUs", double(m_inUs) }, { "markOutUs", double(m_outUs) },
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
        tr.eqOn = t.value("eqOn").toBool(false);
        tr.eqLowDb = t.value("eqLowDb").toDouble(0.0);
        tr.eqMidDb = t.value("eqMidDb").toDouble(0.0);
        tr.eqHighDb = t.value("eqHighDb").toDouble(0.0);
        tr.compOn = t.value("compOn").toBool(false);
        tr.compThreshDb = t.value("compThreshDb").toDouble(-18.0);
        tr.compRatio = t.value("compRatio").toDouble(2.0);
        tr.compMakeupDb = t.value("compMakeupDb").toDouble(0.0);
        tr.gateOn = t.value("gateOn").toBool(false);
        tr.gateThreshDb = t.value("gateThreshDb").toDouble(-40.0);
        tr.deEssOn = t.value("deEssOn").toBool(false);
        tr.deEssThreshDb = t.value("deEssThreshDb").toDouble(-24.0);
        tr.reverbOn = t.value("reverbOn").toBool(false);
        tr.reverbMix = t.value("reverbMix").toDouble(0.25);
        tr.reverbSize = t.value("reverbSize").toDouble(0.5);
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
        c.transition = cj.value("transition").toString(QStringLiteral("cross"));
        c.bypassTransform = cj.value("bypassTransform").toBool(false);
        c.bypassColor = cj.value("bypassColor").toBool(false);
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
        c.color.kfTemp = kfFromJson(co.value("kfTemp"));
        c.color.kfTint = kfFromJson(co.value("kfTint"));
        c.color.kfSat = kfFromJson(co.value("kfSat"));
        c.color.kfLiftX = kfFromJson(co.value("kfLiftX"));
        c.color.kfLiftY = kfFromJson(co.value("kfLiftY"));
        c.color.kfGammaX = kfFromJson(co.value("kfGammaX"));
        c.color.kfGammaY = kfFromJson(co.value("kfGammaY"));
        c.color.kfGainX = kfFromJson(co.value("kfGainX"));
        c.color.kfGainY = kfFromJson(co.value("kfGainY"));
        const QJsonObject au = cj.value("audio").toObject();
        c.audio.gain = au.value("gain").toDouble(1.0);
        c.audio.pan = au.value("pan").toDouble(0.0);
        c.audio.mute = au.value("mute").toBool();
        c.audio.gainKf = kfFromJson(au.value("gainKf"));
        c.audio.panKf = kfFromJson(au.value("panKf"));
        c.audio.eqOn = au.value("eqOn").toBool(false);
        c.audio.eqLowDb = au.value("eqLowDb").toDouble(0.0);
        c.audio.eqMidDb = au.value("eqMidDb").toDouble(0.0);
        c.audio.eqHighDb = au.value("eqHighDb").toDouble(0.0);
        c.audio.compOn = au.value("compOn").toBool(false);
        c.audio.compThreshDb = au.value("compThreshDb").toDouble(-18.0);
        c.audio.compRatio = au.value("compRatio").toDouble(2.0);
        c.audio.compMakeupDb = au.value("compMakeupDb").toDouble(0.0);
        c.audio.gateOn = au.value("gateOn").toBool(false);
        c.audio.gateThreshDb = au.value("gateThreshDb").toDouble(-40.0);
        c.audio.deEssOn = au.value("deEssOn").toBool(false);
        c.audio.deEssThreshDb = au.value("deEssThreshDb").toDouble(-24.0);
        c.audio.reverbOn = au.value("reverbOn").toBool(false);
        c.audio.reverbMix = au.value("reverbMix").toDouble(0.25);
        c.audio.reverbSize = au.value("reverbSize").toDouble(0.5);
        c.audio.eqLowKf = kfFromJson(au.value("eqLowKf"));
        c.audio.eqMidKf = kfFromJson(au.value("eqMidKf"));
        c.audio.eqHighKf = kfFromJson(au.value("eqHighKf"));
        c.audio.reverbMixKf = kfFromJson(au.value("reverbMixKf"));
        c.audio.compThreshKf = kfFromJson(au.value("compThreshKf"));
        c.audio.compRatioKf = kfFromJson(au.value("compRatioKf"));
        c.audio.compMakeupKf = kfFromJson(au.value("compMakeupKf"));
        c.audio.gateThreshKf = kfFromJson(au.value("gateThreshKf"));
        c.audio.deEssThreshKf = kfFromJson(au.value("deEssThreshKf"));
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
    m_masterGain = qBound(0.0, o.value("masterGain").toDouble(1.0), 4.0);
    m_masterPan = qBound(-1.0, o.value("masterPan").toDouble(0.0), 1.0);
    m_masterLimiterOn = o.value("masterLimiterOn").toBool(false);
    m_masterCeilingDb = qBound(-12.0, o.value("masterCeilingDb").toDouble(-1.0), 0.0);
    m_inUs = qMax<qint64>(0, qint64(o.value("markInUs").toDouble(0)));
    m_outUs = qint64(o.value("markOutUs").toDouble(-1));
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
        AudioClip ac{ c.mediaPath, c.trackIndex, c.startUs, c.durationUs, c.inUs,
                      c.speed, gain, pan, eff, gainKf, c.audio.panKf };
        // Efectos de la pista (iguales para todos sus clips; el motor los aplica al submix).
        ac.eqOn = tr.eqOn; ac.eqLowDb = tr.eqLowDb; ac.eqMidDb = tr.eqMidDb; ac.eqHighDb = tr.eqHighDb;
        ac.compOn = tr.compOn; ac.compThreshDb = tr.compThreshDb;
        ac.compRatio = tr.compRatio; ac.compMakeupDb = tr.compMakeupDb;
        ac.gateOn = tr.gateOn; ac.gateThreshDb = tr.gateThreshDb;
        ac.deEssOn = tr.deEssOn; ac.deEssThreshDb = tr.deEssThreshDb;
        ac.reverbOn = tr.reverbOn; ac.reverbMix = tr.reverbMix; ac.reverbSize = tr.reverbSize;
        // Efectos POR CLIP (aplicados al PCM del clip antes del submix).
        ac.clipEqOn = c.audio.eqOn; ac.clipEqLowDb = c.audio.eqLowDb;
        ac.clipEqMidDb = c.audio.eqMidDb; ac.clipEqHighDb = c.audio.eqHighDb;
        ac.clipCompOn = c.audio.compOn; ac.clipCompThreshDb = c.audio.compThreshDb;
        ac.clipCompRatio = c.audio.compRatio; ac.clipCompMakeupDb = c.audio.compMakeupDb;
        ac.clipGateOn = c.audio.gateOn; ac.clipGateThreshDb = c.audio.gateThreshDb;
        ac.clipDeEssOn = c.audio.deEssOn; ac.clipDeEssThreshDb = c.audio.deEssThreshDb;
        ac.clipReverbOn = c.audio.reverbOn; ac.clipReverbMix = c.audio.reverbMix; ac.clipReverbSize = c.audio.reverbSize;
        ac.eqLowKf = c.audio.eqLowKf; ac.eqMidKf = c.audio.eqMidKf;
        ac.eqHighKf = c.audio.eqHighKf; ac.reverbMixKf = c.audio.reverbMixKf;
        ac.compThreshKf = c.audio.compThreshKf; ac.compRatioKf = c.audio.compRatioKf;
        ac.compMakeupKf = c.audio.compMakeupKf; ac.gateThreshKf = c.audio.gateThreshKf;
        ac.deEssThreshKf = c.audio.deEssThreshKf;
        out.push_back(ac);
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
    // Automatización de efectos de audio por clip (EQ, reverb, compresor, puerta, de-esser).
    if (prop == QLatin1String("clipEqLow") || prop == QLatin1String("clipEqMid")
        || prop == QLatin1String("clipEqHigh") || prop == QLatin1String("clipReverbMix")
        || prop == QLatin1String("clipCompThresh") || prop == QLatin1String("clipCompRatio")
        || prop == QLatin1String("clipCompMakeup") || prop == QLatin1String("clipGate")
        || prop == QLatin1String("clipDeEss")) {
        Clip &c = m_clips[i];
        QVector<Keyframe> *kf = kfListFor(c, prop);
        const double sv = prop == QLatin1String("clipEqLow")  ? c.audio.eqLowDb
                        : prop == QLatin1String("clipEqMid")  ? c.audio.eqMidDb
                        : prop == QLatin1String("clipEqHigh") ? c.audio.eqHighDb
                        : prop == QLatin1String("clipReverbMix") ? c.audio.reverbMix
                        : prop == QLatin1String("clipCompThresh") ? c.audio.compThreshDb
                        : prop == QLatin1String("clipCompRatio")  ? c.audio.compRatio
                        : prop == QLatin1String("clipCompMakeup") ? c.audio.compMakeupDb
                        : prop == QLatin1String("clipGate")       ? c.audio.gateThreshDb
                                                                  : c.audio.deEssThreshDb;
        const qint64 src = srcAtPlayhead(c);
        const qint64 tol = 20000;
        for (int k = 0; k < kf->size(); ++k)
            if (qAbs((*kf)[k].sourceUs - src) < tol) { kf->remove(k); bumpSelection(); emit audioChanged(); return; }
        kf->push_back({ src, evalKf(*kf, sv, src) });
        std::sort(kf->begin(), kf->end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; });
        bumpSelection();
        emit audioChanged();
        return;
    }
    // Keyframes de color (temp/tint/sat, listas propias en Clip::Color).
    if (prop == QLatin1String("temp") || prop == QLatin1String("tint") || prop == QLatin1String("sat")) {
        Clip &c = m_clips[i];
        QVector<Keyframe> &kfc = prop == QLatin1String("temp") ? c.color.kfTemp
                               : prop == QLatin1String("tint") ? c.color.kfTint : c.color.kfSat;
        const double sv = prop == QLatin1String("temp") ? c.color.temp
                        : prop == QLatin1String("tint") ? c.color.tint : c.color.sat;
        const qint64 src = srcAtPlayhead(c);
        const qint64 tol = 20000;
        for (int k = 0; k < kfc.size(); ++k)
            if (qAbs(kfc[k].sourceUs - src) < tol) { kfc.remove(k); bumpSelection(); return; }
        kfc.push_back({ src, evalKf(kfc, sv, src) });
        std::sort(kfc.begin(), kfc.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; });
        bumpSelection();
        return;
    }
    // Ruedas 2D (lift/gamma/gain): un diamante anima las componentes X e Y en pareja.
    if (prop == QLatin1String("lift") || prop == QLatin1String("gamma") || prop == QLatin1String("gain")) {
        Clip &c = m_clips[i];
        QVector<Keyframe> &kx = prop == QLatin1String("lift") ? c.color.kfLiftX
                              : prop == QLatin1String("gamma") ? c.color.kfGammaX : c.color.kfGainX;
        QVector<Keyframe> &ky = prop == QLatin1String("lift") ? c.color.kfLiftY
                              : prop == QLatin1String("gamma") ? c.color.kfGammaY : c.color.kfGainY;
        const double sx = prop == QLatin1String("lift") ? c.color.liftX
                        : prop == QLatin1String("gamma") ? c.color.gammaX : c.color.gainX;
        const double sy = prop == QLatin1String("lift") ? c.color.liftY
                        : prop == QLatin1String("gamma") ? c.color.gammaY : c.color.gainY;
        const qint64 src = srcAtPlayhead(c);
        const qint64 tol = 20000;
        bool removed = false;
        for (int k = 0; k < kx.size(); ++k)
            if (qAbs(kx[k].sourceUs - src) < tol) { kx.remove(k); removed = true; break; }
        for (int k = 0; k < ky.size(); ++k)
            if (qAbs(ky[k].sourceUs - src) < tol) { ky.remove(k); removed = true; break; }
        if (!removed) {
            kx.push_back({ src, evalKf(kx, sx, src) });
            ky.push_back({ src, evalKf(ky, sy, src) });
            auto byTime = [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; };
            std::sort(kx.begin(), kx.end(), byTime);
            std::sort(ky.begin(), ky.end(), byTime);
        }
        bumpSelection();
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

QVector<TimelineModel::Keyframe> *TimelineModel::kfListFor(Clip &c, const QString &prop)
{
    if (prop == QLatin1String("audioGain")) return &c.audio.gainKf;
    if (prop == QLatin1String("audioPan"))  return &c.audio.panKf;
    if (prop == QLatin1String("clipEqLow"))  return &c.audio.eqLowKf;
    if (prop == QLatin1String("clipEqMid"))  return &c.audio.eqMidKf;
    if (prop == QLatin1String("clipEqHigh")) return &c.audio.eqHighKf;
    if (prop == QLatin1String("clipReverbMix")) return &c.audio.reverbMixKf;
    if (prop == QLatin1String("clipCompThresh"))  return &c.audio.compThreshKf;
    if (prop == QLatin1String("clipCompRatio"))   return &c.audio.compRatioKf;
    if (prop == QLatin1String("clipCompMakeup"))  return &c.audio.compMakeupKf;
    if (prop == QLatin1String("clipGate"))        return &c.audio.gateThreshKf;
    if (prop == QLatin1String("clipDeEss"))       return &c.audio.deEssThreshKf;
    if (prop == QLatin1String("temp"))      return &c.color.kfTemp;
    if (prop == QLatin1String("tint"))      return &c.color.kfTint;
    if (prop == QLatin1String("sat"))       return &c.color.kfSat;
    // Ruedas 2D: el nombre de la rueda representa la pareja (lista X como
    // representante para consultas); los componentes también son accesibles.
    if (prop == QLatin1String("lift") || prop == QLatin1String("liftX"))   return &c.color.kfLiftX;
    if (prop == QLatin1String("liftY"))                                    return &c.color.kfLiftY;
    if (prop == QLatin1String("gamma") || prop == QLatin1String("gammaX")) return &c.color.kfGammaX;
    if (prop == QLatin1String("gammaY"))                                   return &c.color.kfGammaY;
    if (prop == QLatin1String("gain") || prop == QLatin1String("gainX"))   return &c.color.kfGainX;
    if (prop == QLatin1String("gainY"))                                    return &c.color.kfGainY;
    double *sv = nullptr;
    return kfVec(c.transform, prop, sv);
}

bool TimelineModel::isKeyframed(const QString &prop) const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return false;
    const QVector<Keyframe> *kf =
        const_cast<TimelineModel *>(this)->kfListFor(const_cast<Clip &>(m_clips.at(i)), prop);
    return kf && !kf->isEmpty();
}

bool TimelineModel::hasKeyframeAtPlayhead(const QString &prop) const
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return false;
    const QVector<Keyframe> *kf =
        const_cast<TimelineModel *>(this)->kfListFor(const_cast<Clip &>(m_clips.at(i)), prop);
    if (!kf) return false;
    const qint64 src = srcAtPlayhead(m_clips[i]);
    for (const Keyframe &k : *kf)
        if (qAbs(k.sourceUs - src) < 20000)
            return true;
    return false;
}

int TimelineModel::keyframeInterpAtPlayhead(const QString &prop)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return -1;
    const QVector<Keyframe> *kf = kfListFor(m_clips[i], prop);
    if (!kf) return -1;
    const qint64 src = srcAtPlayhead(m_clips[i]);
    for (const Keyframe &k : *kf)
        if (qAbs(k.sourceUs - src) < 20000)
            return k.interp;
    return -1;
}

void TimelineModel::cycleKeyframeInterp(const QString &prop)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0) return;
    QVector<Keyframe> *kf = kfListFor(m_clips[i], prop);
    if (!kf) return;
    const qint64 src = srcAtPlayhead(m_clips[i]);
    for (Keyframe &k : *kf) {
        if (qAbs(k.sourceUs - src) < 20000) {
            k.interp = (k.interp + 1) % 3;   // lineal → hold → suave → lineal
            // Las ruedas 2D animan X e Y en pareja: sincroniza el interp de la Y.
            if (prop == QLatin1String("lift") || prop == QLatin1String("gamma")
                || prop == QLatin1String("gain")) {
                if (QVector<Keyframe> *ky = kfListFor(m_clips[i], prop + QLatin1String("Y")))
                    for (Keyframe &q : *ky)
                        if (qAbs(q.sourceUs - src) < 20000) { q.interp = k.interp; break; }
            }
            bumpSelection();
            if (prop.startsWith(QLatin1String("audio")))
                emit audioChanged();
            return;
        }
    }
}

QVariantList TimelineModel::keyframePoints(const QString &prop)
{
    QVariantList out;
    const int i = indexOfClip(m_selectedId);
    if (i < 0)
        return out;
    const Clip &c = m_clips.at(i);
    QVector<Keyframe> *kf = kfListFor(m_clips[i], prop);
    if (!kf)
        return out;
    // sourceUs → fracción de la duración del clip en la timeline (puede salir de
    // [0,1] si el keyframe quedó fuera de la ventana visible tras un recorte).
    const double denom = double(c.durationUs) * c.speed;
    for (const Keyframe &k : *kf)
        out.append(QVariantMap{
            { "x", denom > 0 ? double(k.sourceUs - c.inUs) / denom : 0.0 },
            { "v", k.value }, { "interp", k.interp } });
    return out;
}

void TimelineModel::moveKeyframePoint(const QString &prop, int index, double clipFrac, double value)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0)
        return;
    Clip &c = m_clips[i];
    QVector<Keyframe> *kf = kfListFor(c, prop);
    if (!kf || index < 0 || index >= kf->size())
        return;
    clipFrac = qBound(0.0, clipFrac, 1.0);
    (*kf)[index].sourceUs = c.inUs + qint64(clipFrac * c.durationUs * c.speed);
    (*kf)[index].value = value;
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; });
    bumpSelection();
    if (prop.startsWith(QLatin1String("audio")))
        emit audioChanged();
}

void TimelineModel::addKeyframePoint(const QString &prop, double clipFrac, double value)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0)
        return;
    Clip &c = m_clips[i];
    QVector<Keyframe> *kf = kfListFor(c, prop);
    if (!kf)
        return;
    clipFrac = qBound(0.0, clipFrac, 1.0);
    const qint64 src = c.inUs + qint64(clipFrac * c.durationUs * c.speed);
    const qint64 tol = 20000;
    auto byTime = [](const Keyframe &a, const Keyframe &b) { return a.sourceUs < b.sourceUs; };
    // Sobre un keyframe existente (misma tolerancia que el diamante) solo cambia el valor.
    bool updated = false;
    for (Keyframe &k : *kf)
        if (qAbs(k.sourceUs - src) < tol) { k.value = value; updated = true; break; }
    if (!updated) {
        kf->push_back({ src, value });
        std::sort(kf->begin(), kf->end(), byTime);
        // Las ruedas 2D animan X e Y en pareja: mantiene la Y sincronizada en tiempo.
        if (prop == QLatin1String("lift") || prop == QLatin1String("gamma")
            || prop == QLatin1String("gain")) {
            if (QVector<Keyframe> *ky = kfListFor(c, prop + QLatin1String("Y"))) {
                bool has = false;
                for (const Keyframe &q : *ky)
                    if (qAbs(q.sourceUs - src) < tol) { has = true; break; }
                if (!has) {
                    const double sy = prop == QLatin1String("lift") ? c.color.liftY
                                    : prop == QLatin1String("gamma") ? c.color.gammaY : c.color.gainY;
                    ky->push_back({ src, evalKf(*ky, sy, src) });
                    std::sort(ky->begin(), ky->end(), byTime);
                }
            }
        }
    }
    bumpSelection();
    if (prop.startsWith(QLatin1String("audio")))
        emit audioChanged();
}

void TimelineModel::removeKeyframePoint(const QString &prop, int index)
{
    const int i = indexOfClip(m_selectedId);
    if (i < 0)
        return;
    QVector<Keyframe> *kf = kfListFor(m_clips[i], prop);
    if (!kf || index < 0 || index >= kf->size())
        return;
    kf->remove(index);
    bumpSelection();
    if (prop.startsWith(QLatin1String("audio")))
        emit audioChanged();
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

void TimelineModel::nudgeSelected(qint64 deltaUs)
{
    const int idx = indexOfClip(m_selectedId);
    if (idx < 0 || deltaUs == 0)
        return;
    const quint64 id = m_selectedId;
    const qint64 oldStart = m_clips[idx].startUs;
    const qint64 newStart = qBound<qint64>(0, oldStart + deltaUs,
                                           m_totalUs - m_clips[idx].durationUs);
    if (newStart == oldStart)
        return;
    m_undo.push(new TimelineCommand(
        QStringLiteral("Desplazar clip"),
        [this, id, newStart]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].startUs = newStart; emit changed(); }
        },
        [this, id, oldStart]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].startUs = oldStart; emit changed(); }
        }));
}

int TimelineModel::clipCountForMedia(const QString &path) const
{
    if (path.isEmpty())
        return 0;
    int n = 0;
    for (const Clip &c : m_clips)
        if (c.mediaPath == path)
            ++n;
    return n;
}

void TimelineModel::removeClipsWithMedia(const QString &path)
{
    if (path.isEmpty())
        return;
    // Instantánea de los clips afectados con su índice (ascendente) para el undo.
    QVector<QPair<Clip, int>> removed;
    for (int i = 0; i < m_clips.size(); ++i)
        if (m_clips.at(i).mediaPath == path)
            removed.push_back({ m_clips.at(i), i });
    if (removed.isEmpty())
        return;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Eliminar clips del medio"),
        [this, path]() {
            for (int i = m_clips.size() - 1; i >= 0; --i)
                if (m_clips.at(i).mediaPath == path) {
                    if (m_clips.at(i).id == m_selectedId)
                        m_selectedId = 0;
                    doRemoveAt(i);
                }
            emit changed();
        },
        [this, removed]() {
            for (const auto &r : removed)   // índices ascendentes: restaura en orden
                doInsert(r.first, r.second);
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
    growWindow(newStart + dur);   // arrastrar cerca del final extiende la ventana

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

void TimelineModel::applySlide(quint64 id, quint64 aId, quint64 cId, qint64 d)
{
    const int i = indexOfClip(id);
    if (i >= 0)
        m_clips[i].startUs += d;
    if (aId) {                              // vecino anterior: su salida sigue al clip
        const int a = indexOfClip(aId);
        if (a >= 0) m_clips[a].durationUs += d;
    }
    if (cId) {                              // vecino siguiente: su cabeza sigue al clip
        const int c = indexOfClip(cId);
        if (c >= 0) {
            m_clips[c].startUs += d;
            m_clips[c].durationUs -= d;
            m_clips[c].inUs += qint64(d * m_clips[c].speed);
        }
    }
    emit changed();
}

void TimelineModel::slideClip(quint64 id, double deltaFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip cur = m_clips.at(idx);
    qint64 d = qint64(deltaFraction * m_totalUs);
    if (d == 0)
        return;

    // Vecinos ADYACENTES (tolerancia 1 ms): A termina donde empieza el clip y C
    // empieza donde termina; sus bordes siguen al clip (la duración total no cambia).
    const qint64 eps = 1000;
    int ia = -1, ic = -1;
    for (int i = 0; i < m_clips.size(); ++i) {
        const Clip &c = m_clips.at(i);
        if (c.trackIndex != cur.trackIndex || c.id == id)
            continue;
        if (qAbs(c.startUs + c.durationUs - cur.startUs) <= eps) ia = i;
        if (qAbs(c.startUs - (cur.startUs + cur.durationUs)) <= eps) ic = i;
    }

    // Acotaciones: el clip no cruza el origen; A y C conservan la duración mínima;
    // la cabeza de C no retrocede antes del inicio de su origen.
    d = qMax(d, -cur.startUs);
    if (ia >= 0)
        d = qMax(d, kMinClipUs - m_clips[ia].durationUs);
    if (ic >= 0) {
        d = qMin(d, m_clips[ic].durationUs - kMinClipUs);
        d = qMax(d, qint64(-double(m_clips[ic].inUs) / m_clips[ic].speed));
    }
    if (d == 0)
        return;

    const quint64 aId = ia >= 0 ? m_clips[ia].id : 0;
    const quint64 cId = ic >= 0 ? m_clips[ic].id : 0;
    m_undo.push(new TimelineCommand(
        QStringLiteral("Slide"),
        [this, id, aId, cId, d]() { applySlide(id, aId, cId, d); },
        [this, id, aId, cId, d]() { applySlide(id, aId, cId, -d); }));
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

void TimelineModel::rippleTrimLeft(quint64 id, double deltaFraction)
{
    const int idx = indexOfClip(id);
    if (idx < 0)
        return;
    const Clip orig = m_clips.at(idx);
    const qint64 deltaUs = qint64(deltaFraction * m_totalUs);
    if (deltaUs == 0)
        return;

    // El borde de entrada se mueve `applied` (acotado como en trimClip): el clip
    // pierde/gana cabeza pero NO se mueve; los posteriores cierran/abren el hueco.
    qint64 edge = snapUs(orig.startUs + deltaUs, id);
    edge = qBound<qint64>(qMax<qint64>(0, orig.startUs - orig.inUs),
                          edge,
                          orig.startUs + orig.durationUs - kMinClipUs);
    const qint64 applied = edge - orig.startUs;
    if (applied == 0)
        return;
    const qint64 newDur = orig.durationUs - applied;
    const qint64 newIn = orig.inUs + qint64(applied * orig.speed);

    const qint64 origEnd = orig.startUs + orig.durationUs;
    QVector<quint64> shiftIds;
    for (const Clip &c : m_clips)
        if (c.trackIndex == orig.trackIndex && c.id != id && c.startUs >= origEnd)
            shiftIds.push_back(c.id);

    m_undo.push(new TimelineCommand(
        QStringLiteral("Ripple (entrada)"),
        [this, id, newDur, newIn, applied, shiftIds]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].durationUs = newDur; m_clips[i].inUs = newIn; }
            for (quint64 sid : shiftIds) { const int j = indexOfClip(sid); if (j >= 0) m_clips[j].startUs -= applied; }
            emit changed();
        },
        [this, id, od = orig.durationUs, oi = orig.inUs, applied, shiftIds]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].durationUs = od; m_clips[i].inUs = oi; }
            for (quint64 sid : shiftIds) { const int j = indexOfClip(sid); if (j >= 0) m_clips[j].startUs += applied; }
            emit changed();
        }));
}

void TimelineModel::rippleDeleteSelected()
{
    const int idx = indexOfClip(m_selectedId);
    if (idx < 0)
        return;
    const Clip removed = m_clips.at(idx);
    const qint64 removedEnd = removed.startUs + removed.durationUs;
    QVector<quint64> shiftIds;
    for (const Clip &c : m_clips)
        if (c.trackIndex == removed.trackIndex && c.id != removed.id && c.startUs >= removedEnd)
            shiftIds.push_back(c.id);

    m_undo.push(new TimelineCommand(
        QStringLiteral("Eliminar con ripple"),
        [this, rid = removed.id, dur = removed.durationUs, shiftIds]() {
            const int i = indexOfClip(rid);
            if (i >= 0) doRemoveAt(i);
            for (quint64 sid : shiftIds) { const int j = indexOfClip(sid); if (j >= 0) m_clips[j].startUs -= dur; }
            m_selectedId = 0;
            emit changed();
        },
        [this, removed, idx, shiftIds]() {
            doInsert(removed, idx);
            for (quint64 sid : shiftIds) { const int j = indexOfClip(sid); if (j >= 0) m_clips[j].startUs += removed.durationUs; }
            emit changed();
        }));
}

void TimelineModel::setClipTransition(quint64 id, const QString &type)
{
    const int idx = indexOfClip(id);
    if (idx < 0 || m_clips[idx].transition == type)
        return;
    if (type != QLatin1String("cross") && type != QLatin1String("dip") && type != QLatin1String("wipe"))
        return;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Tipo de transición"),
        [this, id, type]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].transition = type; emit changed(); }
        },
        [this, id, old = m_clips[idx].transition]() {
            const int i = indexOfClip(id);
            if (i >= 0) { m_clips[i].transition = old; emit changed(); }
        }));
}

void TimelineModel::setTransitionDuration(quint64 incomingId, double seconds)
{
    const int idx = indexOfClip(incomingId);
    if (idx < 0)
        return;
    const Clip cur = m_clips.at(idx);

    // Clip saliente: el que termina más tarde de los que empiezan antes que el entrante.
    qint64 aEnd = -1, aStart = 0;
    for (const Clip &c : m_clips)
        if (c.trackIndex == cur.trackIndex && c.id != cur.id && c.startUs < cur.startUs) {
            const qint64 e = c.startUs + c.durationUs;
            if (e > aEnd) { aEnd = e; aStart = c.startUs; }
        }
    if (aEnd <= cur.startUs)
        return;   // ya no hay solape sobre el que ajustar

    // Mueve el inicio del entrante para que el solape dure `seconds`.
    qint64 newStart = aEnd - qint64(seconds * 1e6);
    newStart = qBound<qint64>(qMax<qint64>(0, aStart + kMinClipUs), newStart, aEnd - kMinClipUs);
    if (newStart == cur.startUs)
        return;

    m_undo.push(new TimelineCommand(
        QStringLiteral("Duración de transición"),
        [this, incomingId, newStart]() {
            const int i = indexOfClip(incomingId);
            if (i >= 0) { m_clips[i].startUs = newStart; emit changed(); }
        },
        [this, incomingId, os = cur.startUs]() {
            const int i = indexOfClip(incomingId);
            if (i >= 0) { m_clips[i].startUs = os; emit changed(); }
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

// ---- Rango de entrada/salida (marcas I/O) ----
qint64 TimelineModel::exportStartUs() const
{
    return qBound<qint64>(0, m_inUs, qMax<qint64>(0, contentEndUs()));
}
qint64 TimelineModel::exportEndUs() const
{
    const qint64 end = contentEndUs();
    const qint64 out = m_outUs < 0 ? end : qMin(m_outUs, end);
    return qMax(out, exportStartUs());
}
void TimelineModel::setMarkInAtPlayhead()
{
    qint64 us = qBound<qint64>(0, m_playheadUs, contentEndUs());
    if (m_outUs >= 0 && us >= m_outUs) return;   // la entrada debe ir antes de la salida
    if (m_inUs == us) return;
    m_inUs = us;
    emit changed(); emit edited();
}
void TimelineModel::setMarkOutAtPlayhead()
{
    qint64 us = qBound<qint64>(0, m_playheadUs, contentEndUs());
    if (us <= m_inUs) return;                    // la salida debe ir después de la entrada
    if (m_outUs == us) return;
    m_outUs = us;
    emit changed(); emit edited();
}
void TimelineModel::setMarkInFraction(double f)
{
    qint64 us = qBound<qint64>(0, qint64(f * m_totalUs), contentEndUs());
    if (m_outUs >= 0 && us >= m_outUs) us = m_outUs - 1;
    if (us < 0 || m_inUs == us) return;
    m_inUs = us;
    emit changed(); emit edited();
}
void TimelineModel::setMarkOutFraction(double f)
{
    qint64 us = qBound<qint64>(0, qint64(f * m_totalUs), contentEndUs());
    if (us <= m_inUs) us = m_inUs + 1;
    if (m_outUs == us) return;
    m_outUs = us;
    emit changed(); emit edited();
}
void TimelineModel::clearInOut()
{
    if (m_inUs == 0 && m_outUs < 0) return;
    m_inUs = 0; m_outUs = -1;
    emit changed(); emit edited();
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

        // Ripple de ENTRADA: recorta la cabeza (el clip no se mueve) y el posterior retrocede.
        const qint64 st0 = clipById(c0).startUs, in0 = clipById(c0).inUs;
        rippleTrimLeft(c0, df);
        check(clipById(c0).startUs == st0 && clipById(c0).durationUs == d0 - ap
                  && clipById(c0).inUs == in0 + ap,
              "ripple de entrada recorta la cabeza sin mover el clip");
        check(clipById(c1).startUs == s1 - ap, "ripple de entrada retrae el clip posterior");
        undo();
        check(clipById(c0).durationUs == d0 && clipById(c1).startUs == s1,
              "undo de ripple de entrada restaura");

        // Borrado con ripple: el posterior cierra el hueco del eliminado.
        const int nBefore = m_clips.size();
        const qint64 durDel = clipById(c0).durationUs;
        selectClip(c0);
        rippleDeleteSelected();
        check(m_clips.size() == nBefore - 1 && indexOfClip(c0) < 0, "ripple delete elimina el clip");
        check(clipById(c1).startUs == s1 - durDel, "ripple delete cierra el hueco");
        undo();
        check(indexOfClip(c0) >= 0 && clipById(c1).startUs == s1, "undo de ripple delete restaura");
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

        // Tipos de transición: "dip" (fundido por negro) y "wipe" (barrido).
        m_clips[ib].transition = QStringLiteral("dip");
        const qint64 q1 = aEnd - (3 * ov) / 4;   // f = 0.25
        double aOp = -1, bOp2 = -1;
        {
            auto rs = clipsAt(q1);
            for (int k = 0; k < rs.size(); ++k)
                if (rs[k].trackIndex == 2) { if (aOp < 0) aOp = rs[k].transform.opacity; else bOp2 = rs[k].transform.opacity; }
        }
        check(qAbs(aOp - 0.5) < 0.05 && bOp2 < 0.05, "dip f=0.25: saliente ~0.5, entrante 0");

        m_clips[ib].transition = QStringLiteral("wipe");
        double wp = -2;
        for (const auto &r : clipsAt(mid))
            if (r.trackIndex == 2 && r.wipe >= 0) wp = r.wipe;
        check(qAbs(wp - 0.5) < 0.05, "wipe: fraccion de barrido ~0.5 en el medio");

        m_clips[ib].transition = QStringLiteral("cross");
        m_clips[ib].startUs = bStart0;          // restaura
    }

    // 6b) Interpolación de keyframes: hold mantiene, suave = smoothstep.
    {
        QVector<Keyframe> kf{ { 0, 0.0, 1 }, { 1000000, 1.0 } };       // hold
        check(evalKf(kf, 0.0, 500000) == 0.0, "interp hold: mantiene el valor");
        kf[0].interp = 2;                                              // suave
        const double v = evalKf(kf, 0.0, 250000);                      // t=0.25 → 0.15625
        check(qAbs(v - 0.15625) < 1e-9, "interp suave: smoothstep en t=0.25");
        kf[0].interp = 0;
        check(qAbs(evalKf(kf, 0.0, 250000) - 0.25) < 1e-9, "interp lineal: t=0.25");
    }

    // 6c) Keyframes de color: temp animada y una rueda 2D se evalúan en clipsAt.
    {
        const int ci = indexOfClip(v1sorted().first().id);
        const qint64 in0 = m_clips[ci].inUs, dur = m_clips[ci].durationUs;
        m_clips[ci].color.kfTemp = { { in0, 0.0 }, { in0 + dur, 1.0 } };
        m_clips[ci].color.kfLiftY = { { in0, 0.0 }, { in0 + dur, 1.0 } };
        setPlayheadUs(m_clips[ci].startUs + dur / 2);
        double tmp = -1, lY = -1;
        for (const auto &r : clipsAt(playheadUs()))
            if (r.trackIndex == 2) { tmp = r.color.temp; lY = r.color.liftY; }
        check(qAbs(tmp - 0.5) < 0.03, "keyframes de color: temp interpolada ~0.5");
        check(qAbs(lY - 0.5) < 0.03, "keyframes de color: rueda (liftY) interpolada ~0.5");
        m_clips[ci].color.kfTemp.clear();
        m_clips[ci].color.kfLiftY.clear();
        setPlayheadUs(0);
    }

    // 6e) Editor de curvas: puntos, mover y eliminar keyframes por índice.
    {
        const quint64 id = v1sorted().first().id;
        const int ci = indexOfClip(id);
        const qint64 in0 = m_clips[ci].inUs, dur = m_clips[ci].durationUs;
        selectClip(id);
        m_clips[ci].transform.kfOpacity = { { in0, 0.0 }, { in0 + dur, 1.0 } };
        auto pts = keyframePoints(QStringLiteral("opacity"));
        check(pts.size() == 2 && qAbs(pts.at(0).toMap().value("x").toDouble()) < 1e-9
                  && qAbs(pts.at(1).toMap().value("x").toDouble() - 1.0) < 1e-9,
              "curvas: puntos en fracciones 0 y 1");
        moveKeyframePoint(QStringLiteral("opacity"), 1, 0.5, 0.8);
        check(m_clips[ci].transform.kfOpacity.last().sourceUs == in0 + dur / 2
                  && qAbs(m_clips[ci].transform.kfOpacity.last().value - 0.8) < 1e-9,
              "curvas: mover keyframe cambia tiempo y valor");
        removeKeyframePoint(QStringLiteral("opacity"), 1);
        check(m_clips[ci].transform.kfOpacity.size() == 1, "curvas: eliminar keyframe");
        addKeyframePoint(QStringLiteral("opacity"), 0.5, 0.8);
        check(m_clips[ci].transform.kfOpacity.size() == 2
                  && m_clips[ci].transform.kfOpacity.last().sourceUs == in0 + dur / 2
                  && qAbs(m_clips[ci].transform.kfOpacity.last().value - 0.8) < 1e-9,
              "curvas: añadir keyframe en fracción 0.5");
        addKeyframePoint(QStringLiteral("opacity"), 0.5, 0.3);
        check(m_clips[ci].transform.kfOpacity.size() == 2
                  && qAbs(m_clips[ci].transform.kfOpacity.last().value - 0.3) < 1e-9,
              "curvas: añadir sobre un keyframe existente solo cambia el valor");
        // En una rueda 2D añadir a la X crea el par en la Y (misma marca de tiempo).
        selectClip(id);
        addKeyframePoint(QStringLiteral("lift"), 0.25, 0.4);
        check(m_clips[ci].color.kfLiftX.size() == 1 && m_clips[ci].color.kfLiftY.size() == 1
                  && m_clips[ci].color.kfLiftX.first().sourceUs == m_clips[ci].color.kfLiftY.first().sourceUs,
              "curvas: añadir en una rueda 2D sincroniza la pareja Y");
        m_clips[ci].color.kfLiftX.clear();
        m_clips[ci].color.kfLiftY.clear();
        m_clips[ci].transform.kfOpacity.clear();
        selectClip(0);
    }

    // 6d) Slide: el clip se desplaza, A alarga su salida y C recorta su cabeza.
    {
        auto v = v1sorted();
        const quint64 a = v[0].id, b = v[1].id, c = v[2].id;
        // Fuerza adyacencia A|B|C (guardando los inicios originales).
        const qint64 bs0 = clipById(b).startUs, cs0 = clipById(c).startUs;
        m_clips[indexOfClip(b)].startUs = clipById(a).startUs + clipById(a).durationUs;
        m_clips[indexOfClip(c)].startUs = clipById(b).startUs + clipById(b).durationUs;
        const qint64 aDur = clipById(a).durationUs, bStart = clipById(b).startUs, bDur = clipById(b).durationUs;
        const qint64 cStart = clipById(c).startUs, cDur = clipById(c).durationUs, cIn = clipById(c).inUs;
        const double df = 0.02;
        const qint64 ap = qint64(df * m_totalUs);
        slideClip(b, df);
        check(clipById(b).startUs == bStart + ap && clipById(b).durationUs == bDur,
              "slide: el clip se desplaza sin cambiar su duración");
        check(clipById(a).durationUs == aDur + ap, "slide: el anterior alarga su salida");
        check(clipById(c).startUs == cStart + ap && clipById(c).durationUs == cDur - ap
                  && clipById(c).inUs == cIn + ap,
              "slide: el siguiente recorta su cabeza");
        undo();
        check(clipById(a).durationUs == aDur && clipById(b).startUs == bStart
                  && clipById(c).startUs == cStart && clipById(c).inUs == cIn,
              "undo de slide restaura");
        m_clips[indexOfClip(b)].startUs = bs0;   // restaura los inicios originales
        m_clips[indexOfClip(c)].startUs = cs0;
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

    // 9) Modelo QAIM de pistas: una fila por pista y dataChanged granular.
    {
        QAbstractItemModel *tm2 = tracksModel();
        check(tm2->rowCount() == m_tracks.size(), "tracksModel: una fila por pista");
        int changedRow = -1;
        int resets = 0;
        QObject::connect(tm2, &QAbstractItemModel::dataChanged, tm2,
                         [&changedRow](const QModelIndex &tl, const QModelIndex &,
                                       const QList<int> &) { changedRow = tl.row(); });
        QObject::connect(tm2, &QAbstractItemModel::modelReset, tm2,
                         [&resets]() { ++resets; });
        setTrackHidden(1, true);
        check(changedRow == 1 && resets == 0,
              "tracksModel: dataChanged granular solo en la pista cambiada");
        const QVariantMap row = tm2->data(tm2->index(1, 0),
                                          TimelineTracksModel::TrackDataRole).toMap();
        check(row.value("hidden").toBool(), "tracksModel: la fila refleja el cambio");
        setTrackHidden(1, false);
        QObject::disconnect(tm2, nullptr, tm2, nullptr);
    }

    // 10) Medios: contar y eliminar los clips que usan una ruta (con undo).
    {
        const QString mp = QStringLiteral("selftest://medio.mp4");
        const auto v = v1sorted();
        const int nAntes = m_clips.size();
        m_clips[indexOfClip(v[0].id)].mediaPath = mp;
        m_clips[indexOfClip(v[1].id)].mediaPath = mp;
        check(clipCountForMedia(mp) == 2, "clipCountForMedia cuenta los clips del medio");
        removeClipsWithMedia(mp);
        check(m_clips.size() == nAntes - 2 && clipCountForMedia(mp) == 0,
              "removeClipsWithMedia elimina todos los clips del medio");
        undo();
        check(m_clips.size() == nAntes && clipCountForMedia(mp) == 2,
              "undo restaura los clips del medio");
        for (Clip &c : m_clips)   // limpia la ruta para no afectar pruebas posteriores
            if (c.mediaPath == mp)
                c.mediaPath.clear();
    }

    // 11) Nudge (Ctrl+←/→): desplaza el clip seleccionado, con undo y tope en 0.
    {
        const quint64 cid = v1sorted().first().id;   // el primer clip empieza en 0
        selectClip(cid);
        const qint64 st = clipById(cid).startUs;
        nudgeSelected(-40000);   // tope: no puede cruzar el inicio de la secuencia
        check(clipById(cid).startUs == st, "nudge no cruza el inicio de la secuencia");
        nudgeSelected(33366);    // un fotograma a 29.97
        check(clipById(cid).startUs == st + 33366, "nudge derecha desplaza el clip");
        undo();
        check(clipById(cid).startUs == st, "undo del nudge restaura la posición");
    }

    // 12) Gestión de pistas: añadir/eliminar/renombrar con reindexado y undo.
    {
        const int nVid0 = std::count_if(m_tracks.begin(), m_tracks.end(),
                                        [](const Track &t){ return t.kind == "video"; });
        const int nTrk0 = int(m_tracks.size());
        // Un clip de referencia en V1 (índice 2) y su posición actual.
        const quint64 cid = v1sorted().first().id;
        check(clipById(cid).trackIndex == 2, "clip de referencia en V1 (índice 2)");

        // Añadir pista de vídeo (arriba, índice 0): reindexa +1 los clips.
        addVideoTrack();
        check(int(m_tracks.size()) == nTrk0 + 1, "añadir vídeo aumenta el nº de pistas");
        check(m_tracks[0].kind == "video" && m_tracks[0].name == QString("V%1").arg(nVid0 + 1),
              "la nueva pista de vídeo va arriba con nombre único");
        check(clipById(cid).trackIndex == 3, "añadir vídeo arriba reindexa +1 los clips");
        undo();
        check(int(m_tracks.size()) == nTrk0 && clipById(cid).trackIndex == 2,
              "undo de añadir vídeo restaura pistas e índices");

        // Añadir pista de audio (al final): no reindexa clips de vídeo.
        addAudioTrack();
        check(int(m_tracks.size()) == nTrk0 + 1 && m_tracks.last().kind == "audio",
              "añadir audio va al final");
        check(clipById(cid).trackIndex == 2, "añadir audio no reindexa los clips de vídeo");
        undo();
        check(int(m_tracks.size()) == nTrk0, "undo de añadir audio restaura");

        // Renombrar una pista (con undo).
        const QString nm0 = m_tracks[2].name;
        renameTrack(2, "Principal");
        check(m_tracks[2].name == "Principal", "renombrar pista aplica el nombre");
        undo();
        check(m_tracks[2].name == nm0, "undo de renombrar restaura");

        // Eliminar la pista V1 (índice 2): sus clips desaparecen y el resto reindexa;
        // el undo restaura la pista y todos sus clips.
        const int clipsOnV1 = std::count_if(m_clips.begin(), m_clips.end(),
                                            [](const Clip &c){ return c.trackIndex == 2; });
        const int nClips0 = int(m_clips.size());
        removeTrack(2);
        check(int(m_tracks.size()) == nTrk0 - 1, "eliminar pista reduce el nº de pistas");
        check(int(m_clips.size()) == nClips0 - clipsOnV1, "eliminar pista borra sus clips");
        undo();
        check(int(m_tracks.size()) == nTrk0 && int(m_clips.size()) == nClips0,
              "undo de eliminar pista restaura pistas y clips");
        check(m_tracks[2].name == nm0, "undo de eliminar pista restaura la pista correcta");
    }

    // 13) Bypass de nodos (Fusión): saltar Transformar/Color en el compositor.
    {
        const quint64 cid = v1sorted().first().id;   // primer clip de V1 (empieza en 0)
        selectClip(cid);
        setSelBypassTransform(false); setSelBypassColor(false);
        setSelScale(1.5);
        setSelTemp(0.8);
        auto rcFor = [this](quint64 id, qint64 us) -> RenderClip {
            for (const RenderClip &r : clipsAt(us)) if (r.clipId == id) return r;
            return RenderClip{};
        };
        const qint64 tmid = 1000;   // 1 ms: dentro del primer clip, sin solape
        check(qAbs(rcFor(cid, tmid).transform.scale - 1.5) < 1e-6, "el compositor aplica la escala");
        check(qAbs(rcFor(cid, tmid).color.temp - 0.8) < 1e-6, "el compositor aplica el color");
        setSelBypassTransform(true);
        check(qAbs(rcFor(cid, tmid).transform.scale - 1.0) < 1e-6, "bypass Transformar → escala identidad");
        check(qAbs(rcFor(cid, tmid).color.temp - 0.8) < 1e-6, "bypass Transformar no toca el color");
        setSelBypassColor(true);
        check(qAbs(rcFor(cid, tmid).color.temp) < 1e-6, "bypass Color → corrección neutra");
        check(selBypassTransform() && selBypassColor(), "las propiedades de bypass reflejan el estado");
        // Restaura para no afectar a otras pruebas.
        setSelBypassTransform(false); setSelBypassColor(false);
        setSelScale(1.0); setSelTemp(0.0);
    }

    // 14) Rango de entrada/salida (marcas I/O) y su resolución para exportar.
    {
        clearInOut();
        check(!hasInOut(), "sin marcas: rango = contenido completo");
        check(exportStartUs() == 0 && exportEndUs() == contentEndUs(), "rango por defecto = [0, fin]");
        const qint64 end = contentEndUs();
        setPlayheadUs(end / 4);     setMarkInAtPlayhead();
        setPlayheadUs(3 * end / 4); setMarkOutAtPlayhead();
        check(hasInOut(), "marcas I/O activas");
        check(exportStartUs() == end / 4 && exportEndUs() == 3 * end / 4, "rango resuelto = [in, out]");
        // Una entrada posterior a la salida se rechaza (in < out).
        const qint64 inBefore = markInUs();
        setPlayheadUs(end); setMarkInAtPlayhead();
        check(markInUs() == inBefore, "entrada posterior a la salida se rechaza");
        // Round-trip por el .pvsproj.
        const QJsonObject j = toJson();
        clearInOut();
        fromJson(j);
        check(markInUs() == end / 4 && markOutUs() == 3 * end / 4, "marcas I/O persisten en el .pvsproj");
        clearInOut();
        check(!hasInOut() && exportEndUs() == contentEndUs(), "clearInOut restablece el rango");
    }

    // 15) La ventana crece para abarcar un medio más largo que ella (no lo trunca).
    {
        const qint64 longDur = m_totalUs + 300LL * 1000000;   // 5 min más que la ventana
        const quint64 cid = addMediaClip(QStringLiteral("x_long.mp4"), QStringLiteral("Largo"),
                                         QStringLiteral("video"), longDur, 2, 0.0);
        const int ci = indexOfClip(cid);
        check(ci >= 0 && m_clips[ci].durationUs == longDur, "el medio largo no se trunca");
        check(m_totalUs >= longDur, "la ventana crece para abarcar el medio");
        undo();   // quita el clip (la ventana queda a la nueva escala)
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
