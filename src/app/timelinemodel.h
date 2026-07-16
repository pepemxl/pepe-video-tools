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
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY audioChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(double playheadFraction READ playheadFraction NOTIFY playheadChanged)
    Q_PROPERTY(qint64 playheadUs READ playheadUs NOTIFY playheadChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY snapChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY changed)
    // Subtítulos (.srt)
    Q_PROPERTY(int subtitleCount READ subtitleCount NOTIFY subtitlesChanged)
    Q_PROPERTY(bool subtitlesEnabled READ subtitlesEnabled WRITE setSubtitlesEnabled NOTIFY subtitlesChanged)
    Q_PROPERTY(QString activeSubtitle READ activeSubtitle NOTIFY playheadChanged)
    // Transformación del clip seleccionado (para el Inspector).
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectionChanged)
    Q_PROPERTY(double selOpacity READ selOpacity NOTIFY selectionChanged)
    Q_PROPERTY(double selScale READ selScale NOTIFY selectionChanged)
    Q_PROPERTY(double selPosX READ selPosX NOTIFY selectionChanged)
    Q_PROPERTY(double selPosY READ selPosY NOTIFY selectionChanged)
    Q_PROPERTY(double selRotation READ selRotation NOTIFY selectionChanged)
    // Corrección de color del clip seleccionado.
    Q_PROPERTY(double selLiftX READ selLiftX NOTIFY selectionChanged)
    Q_PROPERTY(double selLiftY READ selLiftY NOTIFY selectionChanged)
    Q_PROPERTY(double selGammaX READ selGammaX NOTIFY selectionChanged)
    Q_PROPERTY(double selGammaY READ selGammaY NOTIFY selectionChanged)
    Q_PROPERTY(double selGainX READ selGainX NOTIFY selectionChanged)
    Q_PROPERTY(double selGainY READ selGainY NOTIFY selectionChanged)
    Q_PROPERTY(double selTemp READ selTemp NOTIFY selectionChanged)
    Q_PROPERTY(double selTint READ selTint NOTIFY selectionChanged)
    Q_PROPERTY(double selSat READ selSat NOTIFY selectionChanged)
    Q_PROPERTY(double selSpeed READ selSpeed NOTIFY selectionChanged)
    // Audio del clip seleccionado.
    Q_PROPERTY(bool selHasAudio READ selHasAudio NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioGain READ selAudioGain NOTIFY selectionChanged)
    Q_PROPERTY(double selPan READ selPan NOTIFY selectionChanged)
    Q_PROPERTY(bool selAudioMute READ selAudioMute NOTIFY selectionChanged)
    // Título del clip seleccionado (para el Inspector).
    Q_PROPERTY(bool selIsTitle READ selIsTitle NOTIFY selectionChanged)
    Q_PROPERTY(QString selTitleText READ selTitleText NOTIFY selectionChanged)
    Q_PROPERTY(double selTitleSize READ selTitleSize NOTIFY selectionChanged)
    Q_PROPERTY(QString selTitleColor READ selTitleColor NOTIFY selectionChanged)
    Q_PROPERTY(int selTitleAlign READ selTitleAlign NOTIFY selectionChanged)
    Q_PROPERTY(bool selTitleBar READ selTitleBar NOTIFY selectionChanged)

public:
    explicit TimelineModel(QObject *parent = nullptr);

    struct Track {
        QString name;       // V3, V2, V1, A1, A2, A3
        QString kind;       // "video" | "audio"
        QString idColor;    // color de la etiqueta
        int height;
        bool mute = false;  // silenciar la pista en la mezcla
        bool solo = false;  // aislar: si alguna pista está en solo, solo suenan las solo
    };
    // Un keyframe: valor de una propiedad en un tiempo de origen (sourceUs).
    struct Keyframe {
        qint64 sourceUs;
        double value;
    };
    // Transformación de una capa en el compositor. Cada propiedad puede animarse con
    // keyframes (anclados al tiempo de origen del clip → estables ante mover/recortar).
    struct Transform {
        double posX = 0.0;      // desplazamiento en fracción del ancho de salida (0 = centrado)
        double posY = 0.0;      // fracción del alto
        double scale = 1.0;     // 1.0 = ajustar al lienzo (fit)
        double rotation = 0.0;  // grados
        double opacity = 1.0;   // 0..1
        double cropL = 0.0, cropT = 0.0, cropR = 0.0, cropB = 0.0; // recorte por borde (0..1)
        QVector<Keyframe> kfPosX, kfPosY, kfScale, kfRotation, kfOpacity;
    };
    // Corrección de color primaria. Las ruedas guardan la posición del punto (x,y) en
    // [-1,1]; el compositor deriva el ajuste RGB. temp/tint en [-1,1], sat en [0,2].
    struct Color {
        double liftX = 0.0, liftY = 0.0;    // Sombras
        double gammaX = 0.0, gammaY = 0.0;  // Medios
        double gainX = 0.0, gainY = 0.0;    // Altas
        double temp = 0.0, tint = 0.0, sat = 1.0;
        bool isIdentity() const {
            return liftX == 0.0 && liftY == 0.0 && gammaX == 0.0 && gammaY == 0.0
                && gainX == 0.0 && gainY == 0.0 && temp == 0.0 && tint == 0.0 && sat == 1.0;
        }
    };
    // Audio de un clip: ganancia lineal (1.0 = 0 dB), pan (-1 izq … +1 der), mute y
    // automatización de ganancia por keyframes (anclada al tiempo de origen).
    struct Audio {
        double gain = 1.0;
        double pan = 0.0;
        bool mute = false;
        QVector<Keyframe> gainKf;
        QVector<Keyframe> panKf;
    };
    // Título de texto renderizado como capa por el compositor (para kind == "title").
    struct Title {
        QString text = QStringLiteral("Título");
        double sizePt = 0.09;                        // fracción de la altura de salida
        QString color = QStringLiteral("#ffffff");
        int align = 1;                               // 0=izquierda, 1=centro, 2=derecha
        bool bar = false;                            // barra de fondo (lower third)
        QString barColor = QStringLiteral("#cc0e0f13");
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
        double speed = 1.0;     // remapeo de velocidad (1.0 = normal, 2.0 = 2×, 0.5 = lento)
        Transform transform;
        Color color;
        Audio audio;
        Title title;
    };
    // Instantánea de un clip con audio para la mezcla (consumida por el motor de audio).
    struct AudioClip {
        QString mediaPath;
        int trackIndex;
        qint64 startUs;
        qint64 durationUs;
        qint64 inUs;
        double speed;
        double gain;
        double pan;
        bool mute;
        QVector<Keyframe> gainKf;
        QVector<Keyframe> panKf;
    };
    struct Marker {
        qint64 timeUs;
        QString color;
        QString note;
    };
    // Un subtítulo: intervalo de tiempo + texto (puede ser multilínea).
    struct Subtitle {
        qint64 startUs;
        qint64 endUs;
        QString text;
    };
    // Clip resuelto para el compositor en un instante dado.
    struct RenderClip {
        int trackIndex;
        QString kind;       // "video" | "title"
        QString fill;       // color placeholder si no hay media
        QString mediaPath;  // vacío = sin media (se usa el color)
        qint64 sourceUs;    // tiempo dentro del origen (inUs + offset)
        Transform transform;
        Color color;
        Title title;        // texto del título (si kind == "title")
    };

    QVariantList tracks() const;
    QVariantList audioTracks() const;   // estado mute/solo por pista (para el mezclador)
    QVariantList markers() const;
    // Clips de vídeo activos en el tiempo us, ordenados de abajo (V1) a arriba (V3)
    // para pintarlos en ese orden. Uso del compositor (no expuesto a QML).
    QVector<RenderClip> clipsAt(qint64 us) const;
    // Instantánea de todos los clips con audio (media no vacía). Uso del motor de audio.
    QVector<AudioClip> audioClips() const;
    double playheadFraction() const { return m_totalUs > 0 ? double(m_playheadUs) / m_totalUs : 0.0; }
    qint64 playheadUs() const { return m_playheadUs; }
    qint64 totalUs() const { return m_totalUs; }
    // Fin del contenido: mayor (inicio + duración) entre todos los clips.
    qint64 contentEndUs() const;
    bool snapEnabled() const { return m_snap; }
    void setSnapEnabled(bool on);
    bool canUndo() const { return m_undo.canUndo(); }
    bool canRedo() const { return m_undo.canRedo(); }
    int clipCount() const { return int(m_clips.size()); }
    int subtitleCount() const { return int(m_subtitles.size()); }
    bool subtitlesEnabled() const { return m_subsEnabled; }
    void setSubtitlesEnabled(bool on);
    QString activeSubtitle() const;
    // Parseo/serialización SRT (estáticos, para pruebas).
    static QVector<Subtitle> parseSrt(const QString &content);
    static QString serializeSrt(const QVector<Subtitle> &subs);

    bool hasSelection() const { return indexOfClip(m_selectedId) >= 0; }
    QString selectedName() const;
    double selOpacity() const;
    double selScale() const;
    double selPosX() const;
    double selPosY() const;
    double selRotation() const;
    double selLiftX() const;
    double selLiftY() const;
    double selGammaX() const;
    double selGammaY() const;
    double selGainX() const;
    double selGainY() const;
    double selTemp() const;
    double selTint() const;
    double selSat() const;
    double selSpeed() const;
    bool selHasAudio() const;
    double selAudioGain() const;
    double selPan() const;
    bool selAudioMute() const;
    bool selIsTitle() const;
    QString selTitleText() const;
    double selTitleSize() const;
    QString selTitleColor() const;
    int selTitleAlign() const;
    bool selTitleBar() const;

    // Edición (con undo/redo)
    Q_INVOKABLE void selectClip(quint64 id);
    // Asigna un archivo de medios a un clip (para que el compositor lo decodifique).
    Q_INVOKABLE void setClipMedia(quint64 id, const QString &path);

    // Edición de la transformación del clip seleccionado (recompón sin rehacer la timeline).
    // Si la propiedad está animada, el valor se aplica al keyframe del playhead.
    Q_INVOKABLE void setSelOpacity(double v);
    Q_INVOKABLE void setSelScale(double v);
    Q_INVOKABLE void setSelPosX(double v);
    Q_INVOKABLE void setSelPosY(double v);
    Q_INVOKABLE void setSelRotation(double v);

    // Keyframes de la propiedad indicada ("posX"|"posY"|"scale"|"rotation"|"opacity").
    Q_INVOKABLE void toggleKeyframe(const QString &prop); // añade/quita en el playhead
    Q_INVOKABLE bool isKeyframed(const QString &prop) const;
    Q_INVOKABLE bool hasKeyframeAtPlayhead(const QString &prop) const;

    // Corrección de color del clip seleccionado (ruedas + temp/tint/sat).
    Q_INVOKABLE void setSelLift(double x, double y);
    Q_INVOKABLE void setSelGamma(double x, double y);
    Q_INVOKABLE void setSelGain(double x, double y);
    Q_INVOKABLE void setSelTemp(double v);
    Q_INVOKABLE void setSelTint(double v);
    Q_INVOKABLE void setSelSat(double v);
    Q_INVOKABLE void resetSelColor();
    Q_INVOKABLE void setSelSpeed(double v);   // remapeo de velocidad del clip seleccionado
    // Audio del clip seleccionado. La ganancia respeta la automatización (aplica al keyframe
    // del playhead si está animada, igual que la transformación).
    Q_INVOKABLE void setSelAudioGain(double v);
    Q_INVOKABLE void setSelPan(double v);
    Q_INVOKABLE void setSelAudioMute(bool m);
    // Mute/solo por pista (índice de pista). Afectan a la mezcla completa.
    Q_INVOKABLE void setTrackMute(int trackIndex, bool m);
    Q_INVOKABLE void setTrackSolo(int trackIndex, bool s);
    // Título del clip seleccionado.
    Q_INVOKABLE void setSelTitleText(const QString &text);
    Q_INVOKABLE void setSelTitleSize(double v);
    Q_INVOKABLE void setSelTitleColor(const QString &color);
    Q_INVOKABLE void setSelTitleAlign(int align);
    Q_INVOKABLE void setSelTitleBar(bool bar);
    // Inserta un nuevo clip de título en la pista V3 (índice 0) en el playhead.
    Q_INVOKABLE void addTitleAtPlayhead();

    // Subtítulos (.srt): importar/exportar (con diálogo nativo en Windows) y toggle.
    Q_INVOKABLE bool importSrt(const QString &path);
    Q_INVOKABLE bool exportSrt(const QString &path);
    Q_INVOKABLE void openImportSrtDialog();
    Q_INVOKABLE void openExportSrtDialog();
    Q_INVOKABLE void splitAtFraction(quint64 id, double timelineFraction);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void moveClipToFraction(quint64 id, int trackIndex, double startFraction);
    // Recorta el clip arrastrando un borde. leftEdge = borde izquierdo (entrada);
    // deltaFraction es el desplazamiento del borde en fracción de la ventana total.
    Q_INVOKABLE void trimClip(quint64 id, bool leftEdge, double deltaFraction);
    // Ripple: recorta el borde de salida (derecho) y desplaza los clips posteriores de
    // la misma pista para cerrar/abrir el hueco.
    Q_INVOKABLE void rippleTrimRight(quint64 id, double deltaFraction);
    // Roll: mueve la frontera entre este clip y su vecino adyacente (uno cede lo que el
    // otro gana), sin mover el resto de clips ni alterar la duración total.
    Q_INVOKABLE void rollEdit(quint64 id, bool leftEdge, double deltaFraction);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Marcadores
    Q_INVOKABLE void addMarkerAtPlayhead();
    Q_INVOKABLE void addMarkerAtFraction(double f);
    Q_INVOKABLE void removeMarkerNear(double f);

    // Playhead
    Q_INVOKABLE void setPlayheadFraction(double f);
    Q_INVOKABLE void setPlayheadUs(qint64 us);

signals:
    void changed();
    void playheadChanged();
    void markersChanged();
    void snapChanged();
    void selectionChanged();
    void audioChanged();       // cambió algo relevante para la mezcla de audio (rehornear)
    void subtitlesChanged();   // cambió la lista de subtítulos (recompón el PROGRAMA)

private:
    friend class TimelineCommand;
    void seed();
    int indexOfClip(quint64 id) const;
    void doInsert(const Clip &c, int at);
    Clip doRemoveAt(int at);
    // Ajusta una marca de tiempo al borde de clip / playhead / marcador más cercano
    // dentro de una tolerancia, si el imán (snap) está activo. excludeId se ignora.
    qint64 snapUs(qint64 us, quint64 excludeId) const;
    void runSelfTestIfRequested();

    // --- Keyframes ---
    // Interpola linealmente el valor de una propiedad en sourceUs; si no hay keyframes
    // devuelve el valor estático.
    static double evalKf(const QVector<Keyframe> &kf, double staticVal, qint64 sourceUs);
    // Devuelve los punteros a la lista de keyframes y al valor estático de una propiedad.
    QVector<Keyframe> *kfVec(Transform &tf, const QString &prop, double *&staticOut);
    const QVector<Keyframe> *kfVec(const Transform &tf, const QString &prop) const;
    // Tiempo de origen del clip en el playhead actual (con remapeo de velocidad).
    qint64 srcAtPlayhead(const Clip &c) const { return c.inUs + qint64((m_playheadUs - c.startUs) * c.speed); }
    void bumpSelection();  // ++revisión y emite selectionChanged

    QVector<Track> m_tracks;
    QVector<Clip> m_clips;
    QVector<Marker> m_markers;
    QVector<Subtitle> m_subtitles;
    bool m_subsEnabled = true;
    qint64 m_totalUs = 300LL * 1000000; // ventana de 5 min
    qint64 m_playheadUs = 0;
    quint64 m_selectedId = 0;
    quint64 m_nextId = 1;
    bool m_snap = true;
    QUndoStack m_undo;

    static constexpr qint64 kMinClipUs = 200000; // duración mínima de clip: 0.2 s
};

// Permite pasar RenderClip (y QVector<RenderClip>) por señales encoladas entre hilos.
Q_DECLARE_METATYPE(TimelineModel::RenderClip)
