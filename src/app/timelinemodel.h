#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QUndoStack>

// Modelo de la línea de tiempo (Fase 2): pistas y clips, edición no destructiva
// con undo/redo. Se expone a QML como singleton PepeVideo.TimelineModel.
class TimelineModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)
    Q_PROPERTY(double playheadFraction READ playheadFraction NOTIFY playheadChanged)
    Q_PROPERTY(qint64 playheadUs READ playheadUs NOTIFY playheadChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY changed)

public:
    explicit TimelineModel(QObject *parent = nullptr);

    struct Track {
        QString name;       // V3, V2, V1, A1, A2, A3
        QString kind;       // "video" | "audio"
        QString idColor;    // color de la etiqueta
        int height;
    };
    struct Clip {
        quint64 id;
        int trackIndex;
        QString name;
        QString kind;       // "video" | "title" | "audio"
        QString fill;
        QString border;
        QString wav;        // color de onda (audio) o ""
        qint64 startUs;
        qint64 durationUs;
        qint64 inUs;
        QString mediaPath;
    };

    QVariantList tracks() const;
    double playheadFraction() const { return m_totalUs > 0 ? double(m_playheadUs) / m_totalUs : 0.0; }
    qint64 playheadUs() const { return m_playheadUs; }
    bool canUndo() const { return m_undo.canUndo(); }
    bool canRedo() const { return m_undo.canRedo(); }
    int clipCount() const { return int(m_clips.size()); }

    // Edición (con undo/redo)
    Q_INVOKABLE void selectClip(quint64 id);
    Q_INVOKABLE void splitAtFraction(quint64 id, double timelineFraction);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void moveClipToFraction(quint64 id, int trackIndex, double startFraction);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Playhead
    Q_INVOKABLE void setPlayheadFraction(double f);

signals:
    void changed();
    void playheadChanged();

private:
    friend class TimelineCommand;
    void seed();
    int indexOfClip(quint64 id) const;
    void doInsert(const Clip &c, int at);
    Clip doRemoveAt(int at);
    void runSelfTestIfRequested();

    QVector<Track> m_tracks;
    QVector<Clip> m_clips;
    qint64 m_totalUs = 300LL * 1000000; // ventana de 5 min
    qint64 m_playheadUs = 0;
    quint64 m_selectedId = 0;
    quint64 m_nextId = 1;
    QUndoStack m_undo;
};
