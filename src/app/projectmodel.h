#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QTimer;
class TimelineModel;
class MediaPoolModel;

// Estado del proyecto (Fase 7): nombre/ruta del archivo .pvsproj, guardar/abrir con
// diálogo nativo, autoguardado periódico y marca de cambios sin guardar ("sucio").
// Serializa el timeline completo (TimelineModel::toJson), los ajustes de secuencia
// y las rutas del Media Pool. Se expone a QML como singleton PepeVideo.Project.
class ProjectModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString displayName READ displayName NOTIFY projectChanged)
    Q_PROPERTY(QString baseName READ baseName NOTIFY projectChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY projectChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    // Última escritura a disco: "HH:mm" (vacío = aún sin guardar) y si fue automática.
    Q_PROPERTY(QString lastSavedText READ lastSavedText NOTIFY savedChanged)
    Q_PROPERTY(bool lastSaveAuto READ lastSaveAuto NOTIFY savedChanged)
    Q_PROPERTY(bool autosaveEnabled READ autosaveEnabled WRITE setAutosaveEnabled NOTIFY autosaveChanged)
    // Ajustes de secuencia (StatusBar). Persisten en el proyecto.
    Q_PROPERTY(int seqWidth READ seqWidth NOTIFY sequenceChanged)
    Q_PROPERTY(int seqHeight READ seqHeight NOTIFY sequenceChanged)
    Q_PROPERTY(double seqFps READ seqFps NOTIFY sequenceChanged)
    Q_PROPERTY(QString seqColorSpace READ seqColorSpace NOTIFY sequenceChanged)

public:
    explicit ProjectModel(QObject *parent = nullptr);

    // Conecta las fuentes y captura la instantánea inicial (para "Nuevo proyecto").
    // `pool` puede ser nullptr (pruebas sin Media Pool).
    void setSources(TimelineModel *timeline, MediaPoolModel *pool);

    QString displayName() const;
    QString baseName() const;
    QString filePath() const { return m_filePath; }
    bool dirty() const { return m_dirty; }
    QString lastSavedText() const { return m_lastSavedText; }
    bool lastSaveAuto() const { return m_lastSaveAuto; }
    bool autosaveEnabled() const { return m_autosaveEnabled; }
    void setAutosaveEnabled(bool on);
    int seqWidth() const { return m_seqW; }
    int seqHeight() const { return m_seqH; }
    double seqFps() const { return m_seqFps; }
    QString seqColorSpace() const { return m_seqCS; }

    // Guardar en la ruta actual (diálogo "Guardar como" si aún no hay ruta).
    Q_INVOKABLE bool save();
    Q_INVOKABLE void openSaveAsDialog();
    Q_INVOKABLE void openOpenDialog();
    // Restaura la instantánea inicial (proyecto en blanco de la sesión) sin ruta.
    Q_INVOKABLE void newProject();

    // Núcleo sin diálogos (usado por los diálogos, el autoguardado y las pruebas).
    bool saveToPath(const QString &path, bool autosaved = false);
    bool loadFromPath(const QString &path);

signals:
    void projectChanged();   // nombre / ruta
    void dirtyChanged();
    void savedChanged();     // hora del último guardado
    void autosaveChanged();
    void sequenceChanged();

private:
    void markDirty();
    void clearDirty();
    void noteSaved(bool autosaved);
    QJsonObject projectJson() const;
    bool applyProjectJson(const QJsonObject &o);

    TimelineModel *m_timeline = nullptr;
    MediaPoolModel *m_pool = nullptr;
    QTimer *m_autosave = nullptr;
    QJsonObject m_initial;        // instantánea al arrancar (para "Nuevo proyecto")
    QString m_filePath;
    QString m_lastSavedText;
    bool m_lastSaveAuto = false;
    bool m_dirty = false;
    bool m_loading = false;       // ignora señales de edición durante la carga
    bool m_autosaveEnabled = true;
    int m_seqW = 1920, m_seqH = 1080;
    double m_seqFps = 29.97;
    QString m_seqCS = QStringLiteral("Rec.709");

    static constexpr int kAutosaveMs = 60000;  // 1 min
};

// Auto-test (PVS_PROJ_SELFTEST). Devuelve 0 (OK) / 1 (fallo) / -1 (no solicitado).
int runProjectSelfTestIfRequested();
