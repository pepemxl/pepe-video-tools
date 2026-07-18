#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QUndoStack>

class TimelineTracksModel;

// Modelo de la línea de tiempo (Fase 2): pistas y clips, edición no destructiva
// con undo/redo. Se expone a QML como singleton PepeVideo.TimelineModel.
class TimelineModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY audioChanged)
    // Bus principal (MAIN): ganancia (1.0 = 0 dB) y pan (-1 izq … +1 der) del master.
    Q_PROPERTY(double masterGain READ masterGain NOTIFY audioChanged)
    Q_PROPERTY(double masterPan READ masterPan NOTIFY audioChanged)
    // Limitador de pico del master (evita clipping bajo el techo indicado).
    Q_PROPERTY(bool masterLimiterOn READ masterLimiterOn NOTIFY audioChanged)
    Q_PROPERTY(double masterCeilingDb READ masterCeilingDb NOTIFY audioChanged)
    // Variantes QAbstractItemModel de tracks/audioTracks: dataChanged granular por
    // pista, así los delegados del Repeater PERSISTEN entre ediciones (con las
    // QVariantList, cada cambio destruía y recreaba todos los delegados).
    Q_PROPERTY(QAbstractItemModel *tracksModel READ tracksModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *audioTracksModel READ audioTracksModel CONSTANT)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(double playheadFraction READ playheadFraction NOTIFY playheadChanged)
    Q_PROPERTY(qint64 playheadUs READ playheadUs NOTIFY playheadChanged)
    Q_PROPERTY(qint64 contentEndUs READ contentEndUs NOTIFY changed)  // fin del último clip
    Q_PROPERTY(double totalUsMs READ totalUsMs NOTIFY changed)   // duración de la ventana en ms
    // Rango de entrada/salida (marcas I/O): acota la exportación. markOutUs = -1 → hasta
    // el final del contenido. Las fracciones son sobre la ventana total (para la regla).
    Q_PROPERTY(qint64 markInUs READ markInUs NOTIFY changed)
    Q_PROPERTY(qint64 markOutUs READ markOutUs NOTIFY changed)
    Q_PROPERTY(bool hasInOut READ hasInOut NOTIFY changed)
    Q_PROPERTY(qint64 exportStartUs READ exportStartUs NOTIFY changed)
    Q_PROPERTY(qint64 exportEndUs READ exportEndUs NOTIFY changed)
    Q_PROPERTY(double markInFraction READ markInFraction NOTIFY changed)
    Q_PROPERTY(double markOutFraction READ markOutFraction NOTIFY changed)
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
    // Efectos de audio POR CLIP del clip seleccionado (sección Audio del Inspector).
    Q_PROPERTY(bool selAudioEqOn READ selAudioEqOn NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioEqLowDb READ selAudioEqLowDb NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioEqMidDb READ selAudioEqMidDb NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioEqHighDb READ selAudioEqHighDb NOTIFY selectionChanged)
    Q_PROPERTY(bool selAudioCompOn READ selAudioCompOn NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioCompThreshDb READ selAudioCompThreshDb NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioCompRatio READ selAudioCompRatio NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioCompMakeupDb READ selAudioCompMakeupDb NOTIFY selectionChanged)
    Q_PROPERTY(bool selAudioGateOn READ selAudioGateOn NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioGateThreshDb READ selAudioGateThreshDb NOTIFY selectionChanged)
    Q_PROPERTY(bool selAudioDeEssOn READ selAudioDeEssOn NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioDeEssThreshDb READ selAudioDeEssThreshDb NOTIFY selectionChanged)
    Q_PROPERTY(bool selAudioReverbOn READ selAudioReverbOn NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioReverbMix READ selAudioReverbMix NOTIFY selectionChanged)
    Q_PROPERTY(double selAudioReverbSize READ selAudioReverbSize NOTIFY selectionChanged)
    // Título del clip seleccionado (para el Inspector).
    Q_PROPERTY(bool selIsAudio READ selIsAudio NOTIFY selectionChanged)
    // Bypass de nodos del clip seleccionado (página Fusión).
    Q_PROPERTY(bool selBypassTransform READ selBypassTransform NOTIFY selectionChanged)
    Q_PROPERTY(bool selBypassColor READ selBypassColor NOTIFY selectionChanged)

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
        double gain = 1.0;  // ganancia de pista (fader del mezclador), 1.0 = 0 dB
        double pan = 0.0;   // paneo de pista (-1 izq … +1 der)
        bool hidden = false; // pista de vídeo oculta (no se compone)
        bool locked = false; // pista de vídeo bloqueada (no editable)
        // Efectos de audio por pista (insertados sobre el submix de la pista):
        // EQ de 3 bandas (graves/medios/agudos en dB) y compresor.
        bool eqOn = false;
        double eqLowDb = 0.0, eqMidDb = 0.0, eqHighDb = 0.0;
        bool compOn = false;
        double compThreshDb = -18.0, compRatio = 2.0, compMakeupDb = 0.0;
        // Procesadores adicionales de pista: puerta de ruido, de-esser y reverb.
        bool gateOn = false;
        double gateThreshDb = -40.0;
        bool deEssOn = false;
        double deEssThreshDb = -24.0;
        bool reverbOn = false;
        double reverbMix = 0.25, reverbSize = 0.5;
    };
    // Un keyframe: valor de una propiedad en un tiempo de origen (sourceUs).
    // `interp` define la curva HACIA el siguiente keyframe: 0 = lineal,
    // 1 = hold (mantiene el valor), 2 = suave (smoothstep, ease in-out).
    struct Keyframe {
        qint64 sourceUs;
        double value;
        int interp = 0;
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
        // Keyframes de color: temp/tint/sat y las ruedas 2D (cada rueda anima
        // sus componentes X e Y en pareja — un diamante por rueda).
        QVector<Keyframe> kfTemp, kfTint, kfSat;
        QVector<Keyframe> kfLiftX, kfLiftY, kfGammaX, kfGammaY, kfGainX, kfGainY;
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
        // Efectos por clip (insertos sobre el PCM del clip, antes del submix de pista):
        // cadena puerta → EQ → compresor → de-esser → reverb (mismo motor que la pista).
        bool eqOn = false;
        double eqLowDb = 0.0, eqMidDb = 0.0, eqHighDb = 0.0;
        bool compOn = false;
        double compThreshDb = -18.0, compRatio = 2.0, compMakeupDb = 0.0;
        bool gateOn = false; double gateThreshDb = -40.0;
        bool deEssOn = false; double deEssThreshDb = -24.0;
        bool reverbOn = false; double reverbMix = 0.25, reverbSize = 0.5;
        // Automatización por keyframes de efectos: EQ de 3 bandas y mezcla de reverb.
        QVector<Keyframe> eqLowKf, eqMidKf, eqHighKf, reverbMixKf;
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
        // Bypass de etapas del pipeline (página Fusión): al desactivar un nodo, el
        // compositor lo salta. Transformar → geometría identidad (conserva opacidad);
        // Color → corrección neutra.
        bool bypassTransform = false;
        bool bypassColor = false;
        // Tipo de transición cuando este clip ENTRA solapado sobre el anterior:
        // "cross" (disolvencia), "dip" (fundido por negro) o "wipe" (barrido).
        QString transition = QStringLiteral("cross");
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
        // Efectos de la pista a la que pertenece el clip (iguales para todos los
        // clips de la pista; el motor agrupa por pista y los aplica al submix).
        bool eqOn = false;
        double eqLowDb = 0.0, eqMidDb = 0.0, eqHighDb = 0.0;
        bool compOn = false;
        double compThreshDb = -18.0, compRatio = 2.0, compMakeupDb = 0.0;
        // Procesadores adicionales de pista (puerta · de-esser · reverb).
        bool gateOn = false; double gateThreshDb = -40.0;
        bool deEssOn = false; double deEssThreshDb = -24.0;
        bool reverbOn = false; double reverbMix = 0.25, reverbSize = 0.5;
        // Efectos POR CLIP (insertos sobre el PCM del clip, antes del submix).
        bool clipEqOn = false;
        double clipEqLowDb = 0.0, clipEqMidDb = 0.0, clipEqHighDb = 0.0;
        bool clipCompOn = false;
        double clipCompThreshDb = -18.0, clipCompRatio = 2.0, clipCompMakeupDb = 0.0;
        bool clipGateOn = false; double clipGateThreshDb = -40.0;
        bool clipDeEssOn = false; double clipDeEssThreshDb = -24.0;
        bool clipReverbOn = false; double clipReverbMix = 0.25, clipReverbSize = 0.5;
        // Automatización por keyframes de efectos (EQ de 3 bandas y mezcla de reverb).
        QVector<Keyframe> eqLowKf, eqMidKf, eqHighKf, reverbMixKf;
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
        // Barrido (transición "wipe"): fracción [0..1) del ancho visible del clip
        // entrante; -1 = sin barrido (el worker no recorta).
        double wipe = -1.0;
        // Identidad del clip de origen: el worker mantiene un decodificador POR
        // CLIP (no por archivo), para que dos clips del mismo medio en tiempos
        // distintos no se roben la posición del decode secuencial.
        quint64 clipId = 0;
    };
    // Instantánea autónoma del estado de render (pistas + clips + subtítulos), copiada en
    // el hilo de GUI y pasada al worker de exportación para resolver cada fotograma bajo
    // demanda (`clipsAtSnapshot`) sin materializar cientos de miles de listas por adelantado.
    struct RenderSnapshot {
        QVector<Track> tracks;
        QVector<Clip> clips;
        QVector<Subtitle> subtitles;
        bool subsEnabled = true;
    };

    QVariantList tracks() const;
    QVariantList audioTracks() const;   // estado mute/solo por pista (para el mezclador)
    double masterGain() const { return m_masterGain; }
    double masterPan() const { return m_masterPan; }
    bool masterLimiterOn() const { return m_masterLimiterOn; }
    double masterCeilingDb() const { return m_masterCeilingDb; }
    QAbstractItemModel *tracksModel();        // creado perezosamente (hijo del modelo)
    QAbstractItemModel *audioTracksModel();
    QVariantList markers() const;
    // Clips de vídeo activos en el tiempo us, ordenados de abajo (V1) a arriba (V3)
    // para pintarlos en ese orden. Uso del compositor (no expuesto a QML).
    QVector<RenderClip> clipsAt(qint64 us) const;
    // Instantánea de render (copia) para la exportación en streaming, y resolución de un
    // fotograma a partir de una instantánea (pura, segura entre hilos).
    RenderSnapshot renderSnapshot() const;
    static QVector<RenderClip> clipsAtSnapshot(const RenderSnapshot &snap, qint64 us);
    // Instantánea de todos los clips con audio (media no vacía). Uso del motor de audio.
    QVector<AudioClip> audioClips() const;
    double playheadFraction() const { return m_totalUs > 0 ? double(m_playheadUs) / m_totalUs : 0.0; }
    qint64 playheadUs() const { return m_playheadUs; }
    qint64 totalUs() const { return m_totalUs; }
    double totalUsMs() const { return m_totalUs / 1000.0; }
    // Fin del contenido: mayor (inicio + duración) entre todos los clips.
    qint64 contentEndUs() const;
    // Marcas de entrada/salida y rango de exportación resuelto.
    qint64 markInUs() const { return m_inUs; }
    qint64 markOutUs() const { return m_outUs; }
    bool hasInOut() const { return m_inUs > 0 || m_outUs >= 0; }
    qint64 exportStartUs() const;
    qint64 exportEndUs() const;
    double markInFraction() const { return m_totalUs > 0 ? double(m_inUs) / m_totalUs : 0.0; }
    double markOutFraction() const
    { return m_totalUs > 0 ? double(m_outUs < 0 ? contentEndUs() : m_outUs) / m_totalUs : 0.0; }
    Q_INVOKABLE void setMarkInAtPlayhead();
    Q_INVOKABLE void setMarkOutAtPlayhead();
    Q_INVOKABLE void setMarkInFraction(double f);
    Q_INVOKABLE void setMarkOutFraction(double f);
    Q_INVOKABLE void clearInOut();
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

    // Serialización del documento completo (pistas, clips, marcadores, subtítulos,
    // playhead). Usada por el modelo de proyecto (guardar/abrir .pvsproj).
    QJsonObject toJson() const;
    // Restaura el documento desde JSON: reemplaza el estado, limpia el undo y emite
    // todas las señales de refresco. Devuelve false si el JSON no es un proyecto.
    bool fromJson(const QJsonObject &o);

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
    bool selAudioEqOn() const;
    double selAudioEqLowDb() const;
    double selAudioEqMidDb() const;
    double selAudioEqHighDb() const;
    bool selAudioCompOn() const;
    double selAudioCompThreshDb() const;
    double selAudioCompRatio() const;
    double selAudioCompMakeupDb() const;
    bool selAudioGateOn() const;
    double selAudioGateThreshDb() const;
    bool selAudioDeEssOn() const;
    double selAudioDeEssThreshDb() const;
    bool selAudioReverbOn() const;
    double selAudioReverbMix() const;
    double selAudioReverbSize() const;
    bool selIsAudio() const;
    bool selBypassTransform() const;
    bool selBypassColor() const;
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
    // Interpolación del keyframe del playhead: 0 lineal · 1 hold · 2 suave
    // (-1 = no hay keyframe ahí). `cycle` la rota 0→1→2→0.
    Q_INVOKABLE int keyframeInterpAtPlayhead(const QString &prop);
    Q_INVOKABLE void cycleKeyframeInterp(const QString &prop);
    // Editor de curvas: puntos de la propiedad animada del clip seleccionado como
    // [{x: fracción de la duración del clip, v: valor, interp}] ordenados por x.
    // move/remove editan el keyframe `index` de esa lista (con señales de refresco).
    Q_INVOKABLE QVariantList keyframePoints(const QString &prop);
    Q_INVOKABLE void moveKeyframePoint(const QString &prop, int index, double clipFrac, double value);
    Q_INVOKABLE void removeKeyframePoint(const QString &prop, int index);
    // Crea un keyframe en la fracción `clipFrac` del clip con el valor dado (si ya
    // hay uno en ese punto, solo actualiza su valor). Las ruedas 2D sincronizan la Y.
    Q_INVOKABLE void addKeyframePoint(const QString &prop, double clipFrac, double value);

    // Corrección de color del clip seleccionado (ruedas + temp/tint/sat).
    Q_INVOKABLE void setSelLift(double x, double y);
    Q_INVOKABLE void setSelGamma(double x, double y);
    Q_INVOKABLE void setSelGain(double x, double y);
    Q_INVOKABLE void setSelTemp(double v);
    Q_INVOKABLE void setSelTint(double v);
    Q_INVOKABLE void setSelSat(double v);
    Q_INVOKABLE void resetSelColor();
    // Bypass de nodos del clip seleccionado (página Fusión): salta la etapa en el compositor.
    Q_INVOKABLE void setSelBypassTransform(bool bypass);
    Q_INVOKABLE void setSelBypassColor(bool bypass);
    Q_INVOKABLE void setSelSpeed(double v);   // remapeo de velocidad del clip seleccionado
    // Audio del clip seleccionado. La ganancia respeta la automatización (aplica al keyframe
    // del playhead si está animada, igual que la transformación).
    Q_INVOKABLE void setSelAudioGain(double v);
    Q_INVOKABLE void setSelPan(double v);
    Q_INVOKABLE void setSelAudioMute(bool m);
    // Efectos de audio por clip del clip seleccionado (rehornean la mezcla).
    Q_INVOKABLE void setSelAudioEqEnabled(bool on);
    Q_INVOKABLE void setSelAudioEq(double lowDb, double midDb, double highDb);
    Q_INVOKABLE void setSelAudioCompEnabled(bool on);
    Q_INVOKABLE void setSelAudioComp(double threshDb, double ratio, double makeupDb);
    Q_INVOKABLE void setSelAudioGateEnabled(bool on);
    Q_INVOKABLE void setSelAudioGate(double threshDb);
    Q_INVOKABLE void setSelAudioDeEsserEnabled(bool on);
    Q_INVOKABLE void setSelAudioDeEsser(double threshDb);
    Q_INVOKABLE void setSelAudioReverbEnabled(bool on);
    Q_INVOKABLE void setSelAudioReverb(double mix, double size);
    // Mute/solo por pista (índice de pista). Afectan a la mezcla completa.
    Q_INVOKABLE void setTrackMute(int trackIndex, bool m);
    Q_INVOKABLE void setTrackSolo(int trackIndex, bool s);
    // Ganancia/paneo de pista (faders y perillas del mezclador). Rehornean la mezcla.
    Q_INVOKABLE void setTrackGain(int trackIndex, double gain);
    Q_INVOKABLE void setTrackPan(int trackIndex, double pan);
    // Ganancia/paneo del bus MAIN (fader y perilla del canal MAIN del mezclador).
    Q_INVOKABLE void setMasterGain(double gain);
    Q_INVOKABLE void setMasterPan(double pan);
    // Limitador del master: activar/desactivar y techo en dBFS (-12 … 0).
    Q_INVOKABLE void setMasterLimiterOn(bool on);
    Q_INVOKABLE void setMasterCeilingDb(double db);
    // Efectos de audio por pista (consola de la página Audio). Rehornean la mezcla.
    Q_INVOKABLE void setTrackEqEnabled(int trackIndex, bool on);
    Q_INVOKABLE void setTrackEq(int trackIndex, double lowDb, double midDb, double highDb);
    Q_INVOKABLE void setTrackCompEnabled(int trackIndex, bool on);
    Q_INVOKABLE void setTrackComp(int trackIndex, double threshDb, double ratio, double makeupDb);
    // Procesadores adicionales de pista (puerta de ruido · de-esser · reverb).
    Q_INVOKABLE void setTrackGateEnabled(int trackIndex, bool on);
    Q_INVOKABLE void setTrackGate(int trackIndex, double threshDb);
    Q_INVOKABLE void setTrackDeEsserEnabled(int trackIndex, bool on);
    Q_INVOKABLE void setTrackDeEsser(int trackIndex, double threshDb);
    Q_INVOKABLE void setTrackReverbEnabled(int trackIndex, bool on);
    Q_INVOKABLE void setTrackReverb(int trackIndex, double mix, double size);
    // Visibilidad/bloqueo de pista de vídeo (cabecera 👁/🔒). Oculta = no se compone.
    Q_INVOKABLE void setTrackHidden(int trackIndex, bool hidden);
    Q_INVOKABLE void setTrackLocked(int trackIndex, bool locked);
    // Gestión de pistas (undoable). El vídeo se añade arriba (índice 0) y el audio
    // al final, preservando el invariante "todas las de vídeo antes que las de audio".
    // Añadir/eliminar reindexa los clips (cada clip referencia su pista por índice).
    Q_INVOKABLE void addVideoTrack();
    Q_INVOKABLE void addAudioTrack();
    Q_INVOKABLE void removeTrack(int trackIndex);
    Q_INVOKABLE void renameTrack(int trackIndex, const QString &name);
    // Título del clip seleccionado.
    Q_INVOKABLE void setSelTitleText(const QString &text);
    Q_INVOKABLE void setSelTitleSize(double v);
    Q_INVOKABLE void setSelTitleColor(const QString &color);
    Q_INVOKABLE void setSelTitleAlign(int align);
    Q_INVOKABLE void setSelTitleBar(bool bar);
    // Inserta un nuevo clip de título en la pista V3 (índice 0) en el playhead.
    Q_INVOKABLE void addTitleAtPlayhead();
    // Inserta un clip a partir de un medio (arrastrado desde el Media Pool). Ajusta la
    // pista al tipo correcto (vídeo/audio); devuelve el id del clip creado (0 si falla).
    Q_INVOKABLE quint64 addMediaClip(const QString &path, const QString &name,
                                     const QString &kind, qint64 durationUs,
                                     int trackIndex, double startFraction);

    // Subtítulos (.srt): importar/exportar (con diálogo nativo en Windows) y toggle.
    Q_INVOKABLE bool importSrt(const QString &path);
    Q_INVOKABLE bool exportSrt(const QString &path);
    Q_INVOKABLE void openImportSrtDialog();
    Q_INVOKABLE void openExportSrtDialog();
    Q_INVOKABLE void splitAtFraction(quint64 id, double timelineFraction);
    // Corta el clip seleccionado por el playhead (si este cae dentro del clip).
    Q_INVOKABLE void splitSelectedAtPlayhead();
    Q_INVOKABLE void removeSelected();
    // Desplaza el clip seleccionado ±deltaUs en su pista (Ctrl+←/→; sin imán),
    // con undo. El solape resultante con un vecino crea transición, como al
    // arrastrar.
    Q_INVOKABLE void nudgeSelected(qint64 deltaUs);
    // Medios del pool: nº de clips que usan esa ruta, y borrado de TODOS ellos
    // en un solo comando (undoable). Para eliminar un medio del Media Pool.
    Q_INVOKABLE int clipCountForMedia(const QString &path) const;
    Q_INVOKABLE void removeClipsWithMedia(const QString &path);

    // Pila de undo del proyecto. El MediaPool la comparte (setUndoStack) para
    // que Ctrl+Z global también deshaga sus operaciones. Las macros agrupan
    // varios comandos (p. ej. eliminar un medio Y sus clips) en un solo paso.
    QUndoStack *undoStack() { return &m_undo; }
    Q_INVOKABLE void beginUndoMacro(const QString &text) { m_undo.beginMacro(text); }
    Q_INVOKABLE void endUndoMacro() { m_undo.endMacro(); }
    Q_INVOKABLE void moveClipToFraction(quint64 id, int trackIndex, double startFraction);
    // Recorta el clip arrastrando un borde. leftEdge = borde izquierdo (entrada);
    // deltaFraction es el desplazamiento del borde en fracción de la ventana total.
    Q_INVOKABLE void trimClip(quint64 id, bool leftEdge, double deltaFraction);
    // Slip: desliza el contenido del clip (cambia el punto de entrada in) SIN mover su
    // posición ni su duración en la línea de tiempo. deltaFraction en fracción del total.
    Q_INVOKABLE void slipClip(quint64 id, double deltaFraction);
    // Slide (herramienta U): desplaza el clip; los vecinos ADYACENTES le siguen
    // (el anterior alarga/acorta su salida y el siguiente recorta/extiende su
    // cabeza), así la duración total no cambia. Con undo.
    Q_INVOKABLE void slideClip(quint64 id, double deltaFraction);
    // Herramienta de pista: desplaza en bloque TODOS los clips de una pista (útil para
    // abrir/cerrar huecos). deltaFraction en fracción del total; se acota para que ningún
    // clip cruce el origen.
    Q_INVOKABLE void shiftTrack(int trackIndex, double deltaFraction);
    // Herramienta pluma: alterna un keyframe de una propiedad en un punto de la línea de
    // tiempo (fracción). Mueve el playhead ahí, selecciona el clip y conmuta el keyframe.
    Q_INVOKABLE void penToggleKeyframe(quint64 id, double timelineFraction, const QString &prop);
    // Ripple: recorta el borde de salida (derecho) y desplaza los clips posteriores de
    // la misma pista para cerrar/abrir el hueco.
    Q_INVOKABLE void rippleTrimRight(quint64 id, double deltaFraction);
    // Ripple del borde de ENTRADA: recorta la cabeza del clip (que no se mueve) y
    // desplaza los clips posteriores de la pista para cerrar/abrir el hueco.
    Q_INVOKABLE void rippleTrimLeft(quint64 id, double deltaFraction);
    // Borrado con ripple: elimina el clip seleccionado y los clips posteriores de
    // su pista retroceden para cerrar el hueco. Atajo Mayús+Supr.
    Q_INVOKABLE void rippleDeleteSelected();
    // Transición del clip ENTRANTE de un solape: tipo ("cross"|"dip"|"wipe") y
    // duración (mueve el inicio del entrante para ajustar el solape). Con undo.
    Q_INVOKABLE void setClipTransition(quint64 id, const QString &type);
    Q_INVOKABLE void setTransitionDuration(quint64 incomingId, double seconds);
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
    Q_INVOKABLE void goToStart() { setPlayheadUs(0); }
    Q_INVOKABLE void goToEnd() { setPlayheadUs(contentEndUs()); }

signals:
    void changed();
    void playheadChanged();
    void markersChanged();
    void snapChanged();
    void selectionChanged();
    void audioChanged();       // cambió algo relevante para la mezcla de audio (rehornear)
    void subtitlesChanged();   // cambió la lista de subtítulos (recompón el PROGRAMA)
    // Hubo una EDICIÓN del documento (a diferencia de `changed`, que también se
    // emite al seleccionar). La escucha el modelo de proyecto para marcar "sucio".
    void edited();

private:
    friend class TimelineCommand;
    void seed();
    int indexOfClip(quint64 id) const;
    void doInsert(const Clip &c, int at);
    Clip doRemoveAt(int at);
    // Crece la ventana de la línea de tiempo (nunca encoge) para que quepa `neededUs`,
    // con un minuto de margen. La ventana define la escala tiempo↔fracción de la UI.
    void growWindow(qint64 neededUs);
    // Núcleo de la gestión de pistas (compartido por los comandos de undo/redo).
    // insertTrackAt desplaza +1 los clips con trackIndex >= at; removeTrackAt borra
    // los clips de esa pista y desplaza -1 los de trackIndex > at.
    void insertTrackAt(const Track &t, int at);
    void removeTrackAt(int at);
    // Nombre único para una nueva pista del tipo dado ("V4", "A2", …).
    QString uniqueTrackName(const QString &kind) const;
    // Ajusta una marca de tiempo al borde de clip / playhead / marcador más cercano
    // dentro de una tolerancia, si el imán (snap) está activo. excludeId se ignora.
    qint64 snapUs(qint64 us, quint64 excludeId) const;
    void runSelfTestIfRequested();

    // Núcleo puro de clipsAt/clipsAtSnapshot: resuelve los RenderClip de un instante a
    // partir de pistas/clips/subtítulos (sin estado, seguro entre hilos).
    static QVector<RenderClip> resolveClipsAt(const QVector<Track> &tracks,
                                              const QVector<Clip> &clips,
                                              const QVector<Subtitle> &subs,
                                              bool subsEnabled, qint64 us);

    // --- Keyframes ---
    // Interpola linealmente el valor de una propiedad en sourceUs; si no hay keyframes
    // devuelve el valor estático.
    static double evalKf(const QVector<Keyframe> &kf, double staticVal, qint64 sourceUs);
    // Devuelve los punteros a la lista de keyframes y al valor estático de una propiedad.
    QVector<Keyframe> *kfVec(Transform &tf, const QString &prop, double *&staticOut);
    const QVector<Keyframe> *kfVec(const Transform &tf, const QString &prop) const;
    // Lista de keyframes de CUALQUIER propiedad animable de un clip
    // (transform, audioGain/audioPan, temp/tint/sat). nullptr si no existe.
    QVector<Keyframe> *kfListFor(Clip &c, const QString &prop);
    // Aplica el desplazamiento de un slide (d) al clip y a sus vecinos adyacentes.
    void applySlide(quint64 id, quint64 aId, quint64 cId, qint64 d);
    // Tiempo de origen del clip en el playhead actual (con remapeo de velocidad).
    qint64 srcAtPlayhead(const Clip &c) const { return c.inUs + qint64((m_playheadUs - c.startUs) * c.speed); }
    void bumpSelection();  // ++revisión y emite selectionChanged

    QVector<Track> m_tracks;
    QVector<Clip> m_clips;
    QVector<Marker> m_markers;
    QVector<Subtitle> m_subtitles;
    bool m_subsEnabled = true;
    double m_masterGain = 1.0;          // ganancia del bus MAIN (1.0 = 0 dB)
    double m_masterPan = 0.0;           // pan del bus MAIN (-1 … +1)
    bool m_masterLimiterOn = false;     // limitador de pico del master
    double m_masterCeilingDb = -1.0;    // techo del limitador en dBFS
    qint64 m_inUs = 0;                  // marca de entrada (rango de exportación)
    qint64 m_outUs = -1;                // marca de salida (-1 = hasta el fin del contenido)
    qint64 m_totalUs = 300LL * 1000000; // ventana de 5 min
    qint64 m_playheadUs = 0;
    quint64 m_selectedId = 0;
    quint64 m_nextId = 1;
    bool m_snap = true;
    QUndoStack m_undo;
    TimelineTracksModel *m_tracksModel = nullptr;       // perezosos (hijos)
    TimelineTracksModel *m_audioTracksModel = nullptr;

    static constexpr qint64 kMinClipUs = 200000; // duración mínima de clip: 0.2 s
};

// Permite pasar RenderClip (y QVector<RenderClip>) por señales encoladas entre hilos.
Q_DECLARE_METATYPE(TimelineModel::RenderClip)

// Adaptador QAbstractListModel sobre tracks()/audioTracks(): una fila por pista con
// un único rol ("trackData", expuesto también como `modelData` en los delegados por
// ser el único). En cada cambio del timeline compara fila a fila y emite dataChanged
// SOLO para las pistas que cambiaron (reset únicamente si varía el número de pistas):
// los delegados del Repeater persisten en vez de recrearse con cada edición.
class TimelineTracksModel : public QAbstractListModel
{
    Q_OBJECT
public:
    static constexpr int TrackDataRole = Qt::UserRole + 1;

    TimelineTracksModel(TimelineModel *tm, bool audio, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override
    { return parent.isValid() ? 0 : int(m_rows.size()); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override
    { return { { TrackDataRole, QByteArrayLiteral("trackData") } }; }

private slots:
    void refresh();

private:
    TimelineModel *m_tm;
    bool m_audio;
    QVariantList m_rows;
};
