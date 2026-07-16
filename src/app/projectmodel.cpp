#include "projectmodel.h"

#include "mediapoolmodel.h"
#include "timelinemodel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTime>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commdlg.h>
#endif

ProjectModel::ProjectModel(QObject *parent) : QObject(parent)
{
    // Autoguardado: escribe el proyecto en su ruta cada minuto si hay cambios.
    // Sin ruta aún (proyecto nunca guardado) no hace nada: no hay destino elegido.
    m_autosave = new QTimer(this);
    m_autosave->setInterval(kAutosaveMs);
    connect(m_autosave, &QTimer::timeout, this, [this]() {
        if (m_autosaveEnabled && m_dirty && !m_filePath.isEmpty())
            saveToPath(m_filePath, /*autosaved=*/true);
    });
    m_autosave->start();
}

void ProjectModel::setSources(TimelineModel *timeline, MediaPoolModel *pool)
{
    m_timeline = timeline;
    m_pool = pool;
    if (m_timeline) {
        // Ediciones del documento → proyecto sucio. `edited` cubre undo/redo y las
        // ediciones del Inspector; el resto de señales, marcadores/subtítulos/audio.
        connect(m_timeline, &TimelineModel::edited, this, &ProjectModel::markDirty);
        connect(m_timeline, &TimelineModel::audioChanged, this, &ProjectModel::markDirty);
        connect(m_timeline, &TimelineModel::markersChanged, this, &ProjectModel::markDirty);
        connect(m_timeline, &TimelineModel::subtitlesChanged, this, &ProjectModel::markDirty);
    }
    if (m_pool)   // importar un medio ensucia el proyecto (filtrar la vista NO)
        connect(m_pool, &MediaPoolModel::mediaImported, this, &ProjectModel::markDirty);
    m_initial = projectJson();   // instantánea para "Nuevo proyecto"
}

QString ProjectModel::displayName() const
{
    if (m_filePath.isEmpty())
        return QStringLiteral("Sin título.pvsproj");
    return QFileInfo(m_filePath).fileName();
}

QString ProjectModel::baseName() const
{
    if (m_filePath.isEmpty())
        return QStringLiteral("Sin título");
    return QFileInfo(m_filePath).completeBaseName();
}

void ProjectModel::setAutosaveEnabled(bool on)
{
    if (m_autosaveEnabled == on)
        return;
    m_autosaveEnabled = on;
    emit autosaveChanged();
}

void ProjectModel::markDirty()
{
    if (m_loading || m_dirty)
        return;
    m_dirty = true;
    emit dirtyChanged();
}

void ProjectModel::clearDirty()
{
    if (!m_dirty)
        return;
    m_dirty = false;
    emit dirtyChanged();
}

void ProjectModel::noteSaved(bool autosaved)
{
    m_lastSavedText = QTime::currentTime().toString(QStringLiteral("HH:mm"));
    m_lastSaveAuto = autosaved;
    emit savedChanged();
}

QJsonObject ProjectModel::projectJson() const
{
    QJsonObject o{
        { "app", QStringLiteral("PepeVideoStudio") },
        { "version", 1 },
        { "sequence", QJsonObject{
            { "width", m_seqW }, { "height", m_seqH },
            { "fps", m_seqFps }, { "colorSpace", m_seqCS } } } };
    if (m_timeline)
        o.insert("timeline", m_timeline->toJson());
    if (m_pool) {
        QJsonArray media;
        for (const QString &p : m_pool->mediaPaths())
            media.append(p);
        o.insert("media", media);
    }
    return o;
}

bool ProjectModel::applyProjectJson(const QJsonObject &o)
{
    if (o.value("app").toString() != QLatin1String("PepeVideoStudio"))
        return false;

    m_loading = true;
    const QJsonObject seq = o.value("sequence").toObject();
    m_seqW = seq.value("width").toInt(1920);
    m_seqH = seq.value("height").toInt(1080);
    m_seqFps = seq.value("fps").toDouble(29.97);
    m_seqCS = seq.value("colorSpace").toString(QStringLiteral("Rec.709"));

    // Reimporta los medios que falten ANTES del timeline (los clips los referencian).
    if (m_pool)
        for (const QJsonValue &v : o.value("media").toArray()) {
            const QString p = v.toString();
            if (!p.isEmpty() && QFileInfo::exists(p) && !m_pool->containsPath(p))
                m_pool->importPath(p);
        }

    bool ok = true;
    if (m_timeline && o.contains("timeline"))
        ok = m_timeline->fromJson(o.value("timeline").toObject());
    m_loading = false;
    if (!ok)
        return false;

    emit sequenceChanged();
    clearDirty();
    return true;
}

bool ProjectModel::saveToPath(const QString &path, bool autosaved)
{
    if (path.isEmpty())
        return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(projectJson()).toJson(QJsonDocument::Indented));
    f.close();

    if (m_filePath != path) {
        m_filePath = path;
        emit projectChanged();
    }
    clearDirty();
    noteSaved(autosaved);
    return true;
}

bool ProjectModel::loadFromPath(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject() || !applyProjectJson(doc.object()))
        return false;

    m_filePath = path;
    m_lastSavedText.clear();   // aún no se ha escrito nada en esta sesión
    m_lastSaveAuto = false;
    emit projectChanged();
    emit savedChanged();
    return true;
}

bool ProjectModel::save()
{
    if (m_filePath.isEmpty()) {
        openSaveAsDialog();
        return !m_filePath.isEmpty();
    }
    return saveToPath(m_filePath);
}

void ProjectModel::newProject()
{
    applyProjectJson(m_initial);
    m_filePath.clear();
    m_lastSavedText.clear();
    m_lastSaveAuto = false;
    emit projectChanged();
    emit savedChanged();
}

void ProjectModel::openSaveAsDialog()
{
#ifdef Q_OS_WIN
    QVector<wchar_t> buf(4096, 0);
    const QString suggested = displayName();
    lstrcpynW(buf.data(), reinterpret_cast<const wchar_t *>(suggested.utf16()), buf.size());
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Proyecto PepeVideo (*.pvsproj)\0*.pvsproj\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = buf.size();
    ofn.lpstrTitle = L"Guardar proyecto";
    ofn.lpstrDefExt = L"pvsproj";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn))
        saveToPath(QString::fromWCharArray(buf.data()));
#endif
}

void ProjectModel::openOpenDialog()
{
#ifdef Q_OS_WIN
    QVector<wchar_t> buf(4096, 0);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Proyecto PepeVideo (*.pvsproj)\0*.pvsproj\0Todos los archivos\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = buf.size();
    ofn.lpstrTitle = L"Abrir proyecto";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
        loadFromPath(QString::fromWCharArray(buf.data()));
#endif
}

// ---------------------------------------------------------------------------
// Auto-test (PVS_PROJ_SELFTEST)
// ---------------------------------------------------------------------------

int runProjectSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_PROJ_SELFTEST"))
        return -1;

    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        qInfo("[PROJ-SELFTEST] %-52s %s", name, ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    };

    TimelineModel tl;
    ProjectModel proj;
    proj.setSources(&tl, nullptr);
    const QString path = QDir::tempPath() + "/pvs_proj_selftest.pvsproj";

    check(proj.displayName() == QStringLiteral("Sin título.pvsproj"),
          "proyecto nuevo -> 'Sin titulo.pvsproj'");
    check(!proj.dirty(), "proyecto recien creado no esta sucio");

    // 1) Una edición marca el proyecto sucio.
    tl.setTrackGain(3, 2.0);
    check(proj.dirty(), "editar (ganancia de pista) -> sucio");

    // 2) Guardar limpia el estado y fija nombre/hora.
    const QJsonObject before = tl.toJson();
    check(proj.saveToPath(path), "guardar a disco");
    check(!proj.dirty(), "tras guardar ya no esta sucio");
    check(proj.baseName() == QLatin1String("pvs_proj_selftest"), "baseName = nombre del archivo");
    check(!proj.lastSavedText().isEmpty(), "hora de guardado registrada");

    // 3) Modificar y recargar el archivo restaura el estado guardado exacto.
    tl.setTrackGain(3, 0.25);
    tl.addMarkerAtFraction(0.9);
    tl.setPlayheadFraction(0.9);
    check(proj.dirty(), "nuevas ediciones -> sucio otra vez");
    check(proj.loadFromPath(path), "abrir el proyecto guardado");
    check(tl.toJson() == before, "round-trip: el documento restaurado es identico");
    check(!proj.dirty(), "tras abrir no esta sucio");

    // 4) Seleccionar un clip NO ensucia el proyecto (changed sin edicion).
    const QVariantList tracks = tl.tracks();
    quint64 anyId = 0;
    double anyMid = 0.0;   // centro del clip en fracción del timeline (para cortarlo)
    for (const QVariant &tv : tracks) {
        const QVariantList clips = tv.toMap().value("clips").toList();
        if (!clips.isEmpty()) {
            const QVariantMap c = clips.first().toMap();
            anyId = c.value("id").toULongLong();
            anyMid = c.value("x").toDouble() + c.value("w").toDouble() / 2.0;
            break;
        }
    }
    tl.selectClip(anyId);
    check(!proj.dirty(), "seleccionar un clip no ensucia el proyecto");

    // 5) Una edicion estructural (cortar el clip por su centro) si ensucia.
    tl.splitAtFraction(anyId, anyMid);
    check(proj.dirty(), "edicion estructural (cortar) -> sucio");

    // 6) Nuevo proyecto restaura la instantanea inicial y queda sin ruta.
    proj.newProject();
    check(proj.filePath().isEmpty() && !proj.dirty(), "nuevo proyecto: sin ruta y limpio");

    QFile::remove(path);
    qInfo("[PROJ-SELFTEST] resultado: %s (%d fallos)", failures ? "FALLO" : "OK", failures);
    return failures ? 1 : 0;
}
