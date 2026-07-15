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
    emit changed();
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
    right.inUs = orig.inUs + offset;

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
        newInUs = orig.inUs + applied;
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
    const qint64 us = qBound<qint64>(0, qint64(f * m_totalUs), m_totalUs);
    if (us == m_playheadUs)
        return;
    m_playheadUs = us;
    emit playheadChanged();
}

void TimelineModel::runSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_TL_SELFTEST"))
        return;
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
