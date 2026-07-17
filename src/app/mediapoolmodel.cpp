#include "mediapoolmodel.h"
#include "demodata.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUndoCommand>
#include <QUndoStack>

#include <functional>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <commdlg.h>
#endif

namespace {

QString resolveTool(const QString &envVar, const QString &exe)
{
    const QString fromEnv = QProcessEnvironment::systemEnvironment().value(envVar);
    if (!fromEnv.isEmpty())
        return fromEnv;
    return exe; // se resuelve por PATH
}

QString mmss(double seconds)
{
    if (seconds <= 0)
        return QStringLiteral("00:00");
    const int total = int(seconds + 0.5);
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

double parseFrameRate(const QString &avg)
{
    const auto parts = avg.split(u'/');
    if (parts.size() == 2) {
        bool ok1 = false, ok2 = false;
        const double n = parts[0].toDouble(&ok1);
        const double d = parts[1].toDouble(&ok2);
        if (ok1 && ok2 && d != 0.0)
            return n / d;
    }
    return avg.toDouble();
}

QString prettyCodec(const QString &name)
{
    static const QHash<QString, QString> map = {
        {"h264", "H.264"}, {"hevc", "H.265"}, {"vp9", "VP9"}, {"av1", "AV1"},
        {"prores", "ProRes"}, {"mpeg4", "MPEG-4"}, {"aac", "AAC"}, {"mp3", "MP3"},
        {"pcm_s16le", "PCM"}, {"flac", "FLAC"}, {"vorbis", "Vorbis"}, {"opus", "Opus"}
    };
    return map.value(name, name.toUpper());
}

} // namespace

MediaPoolModel::MediaPoolModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_ffprobe = resolveTool(QStringLiteral("PVS_FFPROBE"), QStringLiteral("ffprobe"));
    m_ffmpeg  = resolveTool(QStringLiteral("PVS_FFMPEG"),  QStringLiteral("ffmpeg"));

    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                 + QStringLiteral("/thumbs");
    QDir().mkpath(m_cacheDir);

    // La app arranca con el pool VACÍO. Los bins/medios de demo solo se
    // generan para los autotests que los esperan o con PVS_DEMO=1.
    if (pvsEnvSet("PVS_DEMO") || pvsEnvSet("PVS_POOL_SELFTEST") || pvsEnvSet("PVS_PROJ_SELFTEST"))
        seedDemo();

    // PVS_DEMO=1 importa además los medios reales de LOCAL_DATA/.
    if (pvsEnvSet("PVS_DEMO")) {
        const QStringList locals = pvsLocalDataMedia();
        for (const QString &p : locals)
            importPath(p);
    }

    // Importación automática de demostración (para pruebas): PVS_DEMO_MEDIA=ruta
    const QString demo = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PVS_DEMO_MEDIA"));
    if (!demo.isEmpty())
        importPath(demo);
}

// Paleta de colores para etiquetas de bins (cíclica).
static const char *kBinColors[] = { "#5b8dd6", "#4a9e6b", "#c98a3e", "#8a6bc0", "#c96a6a" };
static constexpr int kBinColorCount = 5;

// Comando genérico del pool (redo/undo como lambdas), como TimelineCommand.
namespace {
class PoolCommand : public QUndoCommand
{
public:
    PoolCommand(const QString &text, std::function<void()> redoFn, std::function<void()> undoFn)
        : m_redo(std::move(redoFn)), m_undo(std::move(undoFn)) { setText(text); }
    void redo() override { m_redo(); }
    void undo() override { m_undo(); }
private:
    std::function<void()> m_redo, m_undo;
};
} // namespace

void MediaPoolModel::pushPoolOp(const QString &text, std::function<void()> op)
{
    if (!m_undo) {   // sin pila compartida (tests unitarios): operación directa
        op();
        return;
    }
    // Instantánea de bins / asignaciones (por id, estable ante reordenes) / bin
    // activo; el undo la restaura tal cual.
    const QVector<MediaBin> bins = m_bins;
    QHash<quint64, int> itemBins;
    for (const MediaItem &it : m_items)
        itemBins.insert(it.id, it.bin);
    const int current = m_currentBin;
    m_undo->push(new PoolCommand(
        text, std::move(op),
        [this, bins, itemBins, current]() {
            m_bins = bins;
            for (MediaItem &it : m_items)
                it.bin = itemBins.value(it.id, -1);
            m_currentBin = current;
            emit binsChanged();
            emit currentBinChanged();
            rebuildVisible();
        }));
}

void MediaPoolModel::seedDemo()
{
    // Bins de muestra + elementos (sin miniatura real) para no arrancar vacío.
    m_bins = {
        { QStringLiteral("Cámara A · Diálogo"), QString::fromLatin1(kBinColors[0]) },
        { QStringLiteral("B-roll · Mercado"),   QString::fromLatin1(kBinColors[1]) },
        { QStringLiteral("Drone"),              QString::fromLatin1(kBinColors[2]) },
        { QStringLiteral("Música"),             QString::fromLatin1(kBinColors[3]) },
    };
    const struct { const char *name; const char *kind; const char *dur; const char *res;
                   const char *fps; const char *codec; const char *br; const char *tex;
                   bool used; int bin; } demo[] = {
        {"A017_mercado",    "video", "00:14", "3840×2160", "29.97", "H.265", "132 Mb/s", "#26303a", false, 1},
        {"A012_entrevista", "video", "02:41", "3840×2160", "29.97", "H.265", "128 Mb/s", "#2a2632", false, 0},
        {"B009_puesto",     "video", "00:22", "3840×2160", "29.97", "H.265", "148 Mb/s", "#26303a", true,  1},
        {"Drone_zocalo",    "video", "01:08", "3840×2160", "29.97", "H.265", "120 Mb/s", "#233028", false, 2},
        {"A020_calle",      "video", "00:37", "3840×2160", "29.97", "H.265", "130 Mb/s", "#26303a", false, 1},
        {"Musica_intro",    "audio", "03:12", "",          "",      "WAV",   "1536 kb/s","#2b2632", false, 3},
    };
    for (const auto &d : demo) {
        MediaItem it;
        it.id = m_nextId++;
        it.name = QString::fromUtf8(d.name);
        it.kind = QString::fromUtf8(d.kind);
        it.duration = QString::fromUtf8(d.dur);
        it.resolution = QString::fromUtf8(d.res);
        it.fps = QString::fromUtf8(d.fps);
        it.codec = QString::fromUtf8(d.codec);
        it.bitrate = QString::fromUtf8(d.br);
        it.tex = QString::fromUtf8(d.tex);
        it.inUse = d.used;
        it.bin = d.bin;
        m_items.push_back(it);
    }
    rebuildVisible();
}

int MediaPoolModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_visible.size());
}

QVariant MediaPoolModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const MediaItem &it = m_items.at(m_visible.at(index.row()));
    switch (role) {
    case NameRole:       return it.name;
    case PathRole:       return it.path;
    case KindRole:       return it.kind;
    case DurationRole:   return it.duration;
    case ResolutionRole: return it.resolution;
    case FpsRole:        return it.fps;
    case CodecRole:      return it.codec;
    case BitrateRole:    return it.bitrate;
    case ThumbRole:      return it.thumb;
    case TexRole:        return it.tex;
    case InUseRole:      return it.inUse;
    case IdRole:         return QVariant::fromValue(it.id);
    default:             return {};
    }
}

QHash<int, QByteArray> MediaPoolModel::roleNames() const
{
    return {
        {NameRole, "nm"}, {PathRole, "path"}, {KindRole, "kind"},
        {DurationRole, "dur"}, {ResolutionRole, "res"}, {FpsRole, "fps"},
        {CodecRole, "codec"}, {BitrateRole, "bitrate"}, {ThumbRole, "thumb"},
        {TexRole, "tex"}, {InUseRole, "used"}, {IdRole, "mid"}
    };
}

void MediaPoolModel::setSelectedIndex(int i)
{
    // `i` es la fila visible; se guarda el índice del elemento (estable ante filtros).
    const int item = m_visible.value(i, -1);
    if (item == m_selected)
        return;
    m_selected = item;
    emit selectedChanged();
}

bool MediaPoolModel::matchesFilter(const MediaItem &it) const
{
    // El bin activo incluye sus bins descendientes (anidado).
    if (m_currentBin >= 0 && (it.bin < 0 || !binIsUnder(it.bin, m_currentBin)))
        return false;
    return m_filter.isEmpty() || it.name.contains(m_filter, Qt::CaseInsensitive);
}

void MediaPoolModel::setFilter(const QString &f)
{
    const QString t = f.trimmed();
    if (m_filter == t)
        return;
    m_filter = t;
    emit filterChanged();
    rebuildVisible();
}

void MediaPoolModel::rebuildVisible()
{
    beginResetModel();
    m_visible.clear();
    for (int i = 0; i < m_items.size(); ++i)
        if (matchesFilter(m_items.at(i)))
            m_visible.push_back(i);
    endResetModel();
    emit countChanged();
    emit selectedChanged();   // la fila visible del seleccionado pudo cambiar
}

QString MediaPoolModel::selectedName() const
{
    if (m_selected < 0 || m_selected >= m_items.size())
        return {};
    const MediaItem &it = m_items.at(m_selected);
    if (!it.path.isEmpty())
        return QFileInfo(it.path).fileName();   // nombre real con extensión
    return it.name + QStringLiteral(".") + it.kind.left(3);
}

QString MediaPoolModel::selectedLine1() const
{
    if (m_selected < 0 || m_selected >= m_items.size())
        return {};
    const MediaItem &it = m_items.at(m_selected);
    QStringList parts;
    if (!it.resolution.isEmpty()) parts << it.resolution;
    if (!it.fps.isEmpty())        parts << it.fps + QStringLiteral(" fps");
    parts << it.codec;
    return parts.join(QStringLiteral(" · "));
}

QString MediaPoolModel::selectedLine2() const
{
    if (m_selected < 0 || m_selected >= m_items.size())
        return {};
    const MediaItem &it = m_items.at(m_selected);
    QStringList parts;
    if (!it.pixFmt.isEmpty()) parts << it.pixFmt;
    if (!it.bitrate.isEmpty()) parts << it.bitrate;
    return parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(" · "));
}

void MediaPoolModel::appendItem(const MediaItem &item)
{
    if (matchesFilter(item)) {
        beginInsertRows({}, int(m_visible.size()), int(m_visible.size()));
        m_items.push_back(item);
        m_visible.push_back(int(m_items.size()) - 1);
        endInsertRows();
        emit countChanged();
    } else {
        m_items.push_back(item);   // invisible con el filtro actual
    }
    emit mediaImported();
}

int MediaPoolModel::rowForId(quint64 id) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).id == id)
            return i;
    return -1;
}

void MediaPoolModel::openImportDialog()
{
#ifdef Q_OS_WIN
    // Buffer para multiselección (dir\0file1\0file2\0\0).
    QVector<wchar_t> buf(32768, 0);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter =
        L"Medios (vídeo/audio/imagen)\0"
        L"*.mp4;*.mov;*.mkv;*.avi;*.webm;*.wav;*.mp3;*.m4a;*.flac;*.aac;*.png;*.jpg;*.jpeg\0"
        L"Todos los archivos\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = buf.size();
    ofn.lpstrTitle = L"Importar medios";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameW(&ofn))
        return; // cancelado

    // Parseo de la respuesta: si hay un solo archivo, buf es la ruta completa.
    // Si hay varios, buf = directorio\0nombre1\0nombre2\0\0.
    const wchar_t *p = buf.data();
    const QString first = QString::fromWCharArray(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        importPath(first); // un único archivo
        return;
    }
    const QString dir = first;
    while (*p != L'\0') {
        const QString name = QString::fromWCharArray(p);
        p += name.size() + 1;
        importPath(dir + QLatin1Char('/') + name);
    }
#endif
}

void MediaPoolModel::importFile(const QUrl &url)
{
    importPath(url.toLocalFile());
}

QStringList MediaPoolModel::mediaPaths() const
{
    QStringList out;
    for (const MediaItem &it : m_items)
        if (!it.path.isEmpty())
            out.append(it.path);
    return out;
}

bool MediaPoolModel::containsPath(const QString &path) const
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (const MediaItem &it : m_items)
        if (!it.path.isEmpty() && QFileInfo(it.path).absoluteFilePath() == abs)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Bins (carpetas)
// ---------------------------------------------------------------------------

bool MediaPoolModel::binIsUnder(int bin, int ancestor) const
{
    // El padre siempre tiene índice menor que el hijo, así que la cadena termina.
    for (int b = bin; b >= 0 && b < m_bins.size(); b = m_bins[b].parent)
        if (b == ancestor)
            return true;
    return false;
}

QVariantList MediaPoolModel::bins() const
{
    // Orden de árbol (DFS): cada bin seguido de sus descendientes, con profundidad.
    QVariantList out;
    std::function<void(int, int)> walk = [&](int parent, int depth) {
        for (int b = 0; b < m_bins.size(); ++b) {
            if (m_bins[b].parent != parent)
                continue;
            int n = 0;   // medios del bin y de sus descendientes
            for (const MediaItem &it : m_items)
                if (it.bin >= 0 && binIsUnder(it.bin, b))
                    ++n;
            out.append(QVariantMap{
                { "index", b }, { "name", m_bins[b].name },
                { "color", m_bins[b].color }, { "count", n }, { "depth", depth } });
            walk(b, depth + 1);
        }
    };
    walk(-1, 0);
    return out;
}

void MediaPoolModel::setCurrentBin(int bin)
{
    bin = qBound(-1, bin, int(m_bins.size()) - 1);
    if (bin == m_currentBin)
        return;
    m_currentBin = bin;
    emit currentBinChanged();
    rebuildVisible();
}

int MediaPoolModel::addBin(const QString &name, int parent)
{
    const int newIndex = int(m_bins.size());   // los bins se añaden al final
    pushPoolOp(QStringLiteral("Crear bin"), [this, name, parent]() {
        MediaBin b;
        b.name = name.trimmed().isEmpty()
                     ? QStringLiteral("Bin %1").arg(m_bins.size() + 1)
                     : name.trimmed();
        b.color = QString::fromLatin1(kBinColors[m_bins.size() % kBinColorCount]);
        b.parent = (parent >= -1 && parent < m_bins.size()) ? parent : -1;
        m_bins.push_back(b);
        emit binsChanged();
    });
    return newIndex;
}

void MediaPoolModel::renameBin(int index, const QString &name)
{
    const QString t = name.trimmed();
    if (index < 0 || index >= m_bins.size() || t.isEmpty() || m_bins[index].name == t)
        return;
    pushPoolOp(QStringLiteral("Renombrar bin"), [this, index, t]() {
        if (index >= 0 && index < m_bins.size()) {
            m_bins[index].name = t;
            emit binsChanged();
        }
    });
}

void MediaPoolModel::removeBin(int index)
{
    if (index < 0 || index >= m_bins.size())
        return;
    pushPoolOp(QStringLiteral("Eliminar bin"), [this, index]() {
        if (index < 0 || index >= m_bins.size())
            return;
        const int promoted = m_bins[index].parent;   // < index (el padre se crea antes)
        m_bins.remove(index);
        // Los hijos del eliminado suben a su padre; los índices superiores bajan.
        for (MediaBin &b : m_bins) {
            if (b.parent == index) b.parent = promoted;
            else if (b.parent > index) --b.parent;
        }
        // Sus medios quedan sin bin; los índices superiores bajan una posición.
        for (MediaItem &it : m_items) {
            if (it.bin == index) it.bin = -1;
            else if (it.bin > index) --it.bin;
        }
        if (m_currentBin == index)
            setCurrentBin(-1);
        else if (m_currentBin > index)
            m_currentBin -= 1;   // mismo bin activo, índice corrido (sin refiltrar)
        emit binsChanged();
        rebuildVisible();
    });
}

void MediaPoolModel::moveToBin(quint64 id, int binIndex)
{
    const int row = rowForId(id);
    if (row < 0 || binIndex < -1 || binIndex >= m_bins.size() || m_items[row].bin == binIndex)
        return;
    pushPoolOp(QStringLiteral("Mover al bin"), [this, id, binIndex]() {
        const int r = rowForId(id);
        if (r < 0 || binIndex < -1 || binIndex >= m_bins.size())
            return;
        m_items[r].bin = binIndex;
        emit binsChanged();
        rebuildVisible();   // puede entrar/salir de la vista del bin activo
    });
}

void MediaPoolModel::removeItem(quint64 id)
{
    const int row = rowForId(id);
    if (row < 0)
        return;
    const MediaItem item = m_items.at(row);   // copia para el undo
    const int selPrev = m_selected;

    auto redoFn = [this, id]() {
        const int r = rowForId(id);
        if (r < 0)
            return;
        m_items.removeAt(r);
        // La selección guarda un índice de m_items: ajústala al desplazamiento.
        if (m_selected == r)
            m_selected = -1;
        else if (m_selected > r)
            --m_selected;
        rebuildVisible();      // reset del modelo + countChanged + selectedChanged
        emit binsChanged();    // los contadores de los bins cambian
        emit mediaRemoved();   // el proyecto se marca sucio
    };
    if (!m_undo) {   // sin pila compartida (tests unitarios): directo
        redoFn();
        return;
    }
    m_undo->push(new PoolCommand(
        QStringLiteral("Eliminar medio"), std::move(redoFn),
        [this, item, row, selPrev]() {
            m_items.insert(qMin(row, int(m_items.size())), item);
            m_selected = selPrev;
            rebuildVisible();
            emit binsChanged();
            emit mediaImported();   // reapareció un medio (el proyecto se ensucia)
        }));
}

QStringList MediaPoolModel::binNames() const
{
    QStringList out;
    for (const MediaBin &b : m_bins)
        out.append(b.name);
    return out;
}

QJsonArray MediaPoolModel::binsJson() const
{
    QJsonArray out;
    for (const MediaBin &b : m_bins)
        out.append(QJsonObject{ { "name", b.name }, { "parent", b.parent } });
    return out;
}

void MediaPoolModel::setBinsJson(const QJsonArray &arr)
{
    m_bins.clear();
    for (const QJsonValue &v : arr) {
        MediaBin b;
        if (v.isObject()) {   // formato actual: { name, parent }
            b.name = v.toObject().value(QStringLiteral("name")).toString();
            b.parent = v.toObject().value(QStringLiteral("parent")).toInt(-1);
        } else {              // formato antiguo: cadena con el nombre (raíz)
            b.name = v.toString();
        }
        if (b.parent < -1 || b.parent >= m_bins.size())
            b.parent = -1;    // solo se admite un padre ya creado (índice menor)
        b.color = QString::fromLatin1(kBinColors[m_bins.size() % kBinColorCount]);
        m_bins.push_back(b);
    }
    // Asignaciones fuera de rango quedan sin bin; la vista vuelve a "todos".
    for (MediaItem &it : m_items)
        if (it.bin >= m_bins.size())
            it.bin = -1;
    m_currentBin = -1;
    emit binsChanged();
    emit currentBinChanged();
    rebuildVisible();
}

void MediaPoolModel::setBins(const QStringList &names)
{
    QJsonArray arr;
    for (const QString &n : names)
        arr.append(n);
    setBinsJson(arr);
}

QString MediaPoolModel::binNameOfPath(const QString &path) const
{
    for (const MediaItem &it : m_items)
        if (!it.path.isEmpty() && it.path == path)
            return it.bin >= 0 && it.bin < m_bins.size() ? m_bins[it.bin].name : QString();
    return {};
}

void MediaPoolModel::setPathBin(const QString &path, int binIndex)
{
    if (binIndex < -1 || binIndex >= m_bins.size())
        return;
    for (int i = 0; i < m_items.size(); ++i)
        if (!m_items[i].path.isEmpty() && m_items[i].path == path) {
            if (m_items[i].bin != binIndex) {
                m_items[i].bin = binIndex;
                emit binsChanged();
                rebuildVisible();
            }
            return;
        }
}

void MediaPoolModel::importPath(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        emit importError(QStringLiteral("No existe: %1").arg(path));
        return;
    }

    // ffprobe -> JSON con format + streams
    QProcess probe;
    probe.start(m_ffprobe, {
        QStringLiteral("-v"), QStringLiteral("quiet"),
        QStringLiteral("-print_format"), QStringLiteral("json"),
        QStringLiteral("-show_format"), QStringLiteral("-show_streams"),
        fi.absoluteFilePath()
    });
    if (!probe.waitForStarted(3000)) {
        emit importError(QStringLiteral("No se pudo ejecutar ffprobe (¿está en el PATH?)"));
        return;
    }
    probe.waitForFinished(8000);

    const QJsonDocument doc = QJsonDocument::fromJson(probe.readAllStandardOutput());
    const QJsonObject root = doc.object();
    const QJsonObject format = root.value(QStringLiteral("format")).toObject();
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();

    MediaItem it;
    it.id = m_nextId++;
    it.name = fi.completeBaseName();
    it.path = fi.absoluteFilePath();
    it.tex = QStringLiteral("#242a2f");

    const double durationSec = format.value(QStringLiteral("duration")).toString().toDouble();
    it.duration = mmss(durationSec);

    const double bitRate = format.value(QStringLiteral("bit_rate")).toString().toDouble();
    if (bitRate > 0)
        it.bitrate = QStringLiteral("%1 Mb/s").arg(bitRate / 1e6, 0, 'f', 1);

    QJsonObject vStream, aStream;
    for (const QJsonValue &v : streams) {
        const QJsonObject s = v.toObject();
        const QString type = s.value(QStringLiteral("codec_type")).toString();
        if (type == QLatin1String("video") && vStream.isEmpty()) vStream = s;
        else if (type == QLatin1String("audio") && aStream.isEmpty()) aStream = s;
    }

    if (!vStream.isEmpty()) {
        it.kind = QStringLiteral("video");
        const int w = vStream.value(QStringLiteral("width")).toInt();
        const int h = vStream.value(QStringLiteral("height")).toInt();
        if (w > 0 && h > 0)
            it.resolution = QStringLiteral("%1×%2").arg(w).arg(h);
        const double r = parseFrameRate(vStream.value(QStringLiteral("avg_frame_rate")).toString());
        if (r > 0)
            it.fps = QString::number(r, 'f', 2);
        it.codec = prettyCodec(vStream.value(QStringLiteral("codec_name")).toString());
        it.pixFmt = vStream.value(QStringLiteral("pix_fmt")).toString();
    } else if (!aStream.isEmpty()) {
        it.kind = QStringLiteral("audio");
        it.codec = prettyCodec(aStream.value(QStringLiteral("codec_name")).toString());
        const int sr = aStream.value(QStringLiteral("sample_rate")).toString().toInt();
        if (sr > 0)
            it.pixFmt = QStringLiteral("%1 kHz").arg(sr / 1000.0, 0, 'f', 1);
    } else {
        it.kind = QStringLiteral("image");
        it.codec = QStringLiteral("IMG");
    }

    appendItem(it);
    setSelectedIndex(int(m_items.size()) - 1); // selecciona el recién importado
    generateThumbnail(it.id, it.path, it.kind, durationSec > 0 ? qMin(1.0, durationSec / 2.0) : 0.0);
}

void MediaPoolModel::generateThumbnail(quint64 id, const QString &path, const QString &kind, double atSeconds)
{
    const QString out = QStringLiteral("%1/%2.png").arg(m_cacheDir).arg(id);

    QStringList args;
    if (kind == QLatin1String("audio")) {
        args << QStringLiteral("-y") << QStringLiteral("-i") << path
             << QStringLiteral("-filter_complex")
             << QStringLiteral("showwavespic=s=320x120:colors=0x8a6bc0")
             << QStringLiteral("-frames:v") << QStringLiteral("1") << out;
    } else {
        args << QStringLiteral("-y")
             << QStringLiteral("-ss") << QString::number(atSeconds, 'f', 3)
             << QStringLiteral("-i") << path
             << QStringLiteral("-frames:v") << QStringLiteral("1")
             << QStringLiteral("-vf") << QStringLiteral("scale=320:-1")
             << out;
    }

    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this,
            [this, proc, id, out](int code, QProcess::ExitStatus) {
        proc->deleteLater();
        if (code != 0 || !QFileInfo::exists(out))
            return;
        const int row = rowForId(id);
        if (row < 0)
            return;
        m_items[row].thumb = QUrl::fromLocalFile(out).toString();
        const int vis = int(m_visible.indexOf(row));   // fila visible (si no está filtrada)
        if (vis >= 0) {
            const QModelIndex idx = index(vis);
            emit dataChanged(idx, idx, {ThumbRole});
        }
        if (row == m_selected)
            emit selectedChanged();
    });
    proc->start(m_ffmpeg, args);
}

// ---------------------------------------------------------------------------
// Auto-test (PVS_POOL_SELFTEST)
// ---------------------------------------------------------------------------

int runPoolSelfTestIfRequested()
{
    if (qEnvironmentVariableIsEmpty("PVS_POOL_SELFTEST"))
        return -1;

    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        qInfo("[POOL-SELFTEST] %-52s %s", name, ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    };

    MediaPoolModel pool;
    check(pool.bins().size() == 4, "arranque: 4 bins de demo");
    check(pool.count() == 6, "arranque: 6 medios visibles (todos)");

    // 1) Filtrar por bin: Drone (indice 2) tiene 1 medio.
    pool.setCurrentBin(2);
    check(pool.count() == 1, "bin Drone activo -> 1 medio visible");
    // Texto + bin se combinan: 'a017' no esta en Drone.
    pool.setFilter(QStringLiteral("a017"));
    check(pool.count() == 0, "bin Drone + filtro 'a017' -> 0");
    pool.setCurrentBin(-1);
    check(pool.count() == 1, "todos + filtro 'a017' -> 1");
    pool.setFilter(QString());

    // 2) Crear un bin y mover un medio (por id) a el.
    const int nuevo = pool.addBin(QStringLiteral("Selectos"));
    check(nuevo == 4 && pool.bins().size() == 5, "addBin crea el 5o bin");
    const quint64 firstId = pool.data(pool.index(0), MediaPoolModel::IdRole).toULongLong();
    pool.moveToBin(firstId, nuevo);
    pool.setCurrentBin(nuevo);
    check(pool.count() == 1, "medio movido al bin nuevo (visible al filtrar)");
    const QVariantMap binInfo = pool.bins().at(nuevo).toMap();
    check(binInfo.value("count").toInt() == 1, "contador del bin nuevo = 1");

    // 3) Eliminar un bin reindexa: el bin 'Selectos' (4) pasa a ser el 3.
    pool.removeBin(0);   // fuera 'Camara A' (su medio queda sin bin)
    check(pool.bins().size() == 4, "removeBin: quedan 4 bins");
    check(pool.currentBin() == nuevo - 1, "bin activo reindexado (4 -> 3)");
    check(pool.count() == 1, "el medio movido sigue visible en su bin");
    pool.setCurrentBin(-1);
    check(pool.count() == 6, "todos: los 6 medios siguen ahi");

    // 4) Persistencia por nombres: setBins restaura la lista y limpia el activo.
    pool.setBins({ QStringLiteral("Uno"), QStringLiteral("Dos") });
    check(pool.binNames() == QStringList({ QStringLiteral("Uno"), QStringLiteral("Dos") }),
          "setBins reemplaza la lista");
    check(pool.currentBin() == -1 && pool.count() == 6, "tras setBins: vista en 'todos'");

    // 5) Renombrar: cambia el nombre; vacio/espacios se ignora.
    pool.renameBin(0, QStringLiteral("Uno bis"));
    check(pool.binNames().first() == QStringLiteral("Uno bis"), "renameBin cambia el nombre");
    pool.renameBin(0, QStringLiteral("   "));
    check(pool.binNames().first() == QStringLiteral("Uno bis"), "renameBin ignora nombre vacio");

    // 6) Bins anidados: hijo de 'Uno bis' (0); DFS con profundidad en bins().
    const int hijo = pool.addBin(QStringLiteral("Hijo"), 0);
    check(hijo == 2 && pool.bins().size() == 3, "addBin anidado crea el bin hijo");
    {
        const QVariantMap fila1 = pool.bins().at(1).toMap();   // tras 'Uno bis'
        check(fila1.value("name") == QStringLiteral("Hijo")
                  && fila1.value("depth").toInt() == 1,
              "bins() en orden DFS con profundidad");
    }
    // Un medio en el hijo cuenta y filtra tambien desde el padre. Primero se
    // sacan todos los medios de sus bins (algunos conservaban el bin 0 de la demo).
    for (int i = 0; i < 6; ++i)
        pool.moveToBin(pool.data(pool.index(i), MediaPoolModel::IdRole).toULongLong(), -1);
    const quint64 idNieto = pool.data(pool.index(0), MediaPoolModel::IdRole).toULongLong();
    pool.moveToBin(idNieto, hijo);
    pool.setCurrentBin(0);
    check(pool.count() == 1, "filtrar el bin padre incluye los medios del hijo");
    check(pool.bins().at(0).toMap().value("count").toInt() == 1,
          "contador del padre agrega los descendientes");

    // 7) Persistencia con anidado: round-trip por JSON conserva el padre.
    pool.setBinsJson(pool.binsJson());
    check(pool.binNames() == QStringList({ QStringLiteral("Uno bis"), QStringLiteral("Dos"),
                                           QStringLiteral("Hijo") })
              && pool.bins().at(1).toMap().value("name") == QStringLiteral("Hijo"),
          "binsJson round-trip conserva nombres y anidado");

    // 8) Eliminar el padre promueve el hijo a la raiz.
    pool.removeBin(0);
    check(pool.bins().size() == 2
              && pool.bins().at(1).toMap().value("name") == QStringLiteral("Hijo")
              && pool.bins().at(1).toMap().value("depth").toInt() == 0,
          "removeBin promueve los hijos a la raiz");

    // 9) Eliminar un medio del pool.
    pool.setCurrentBin(-1);
    const int nAntes = pool.count();
    pool.setSelectedIndex(0);
    const quint64 idDel = pool.data(pool.index(0), MediaPoolModel::IdRole).toULongLong();
    pool.removeItem(idDel);
    check(pool.count() == nAntes - 1, "removeItem: queda un medio menos");
    check(pool.selectedIndex() == -1, "removeItem: la seleccion del eliminado se limpia");
    pool.removeItem(idDel);   // id ya inexistente: no-op
    check(pool.count() == nAntes - 1, "removeItem con id inexistente es no-op");

    // 10) Undo del pool (pila compartida): medios y bins.
    QUndoStack undoStack;
    pool.setUndoStack(&undoStack);
    const int nU = pool.count();
    const quint64 idU = pool.data(pool.index(0), MediaPoolModel::IdRole).toULongLong();
    pool.removeItem(idU);
    check(pool.count() == nU - 1, "removeItem (undoable) elimina el medio");
    undoStack.undo();
    check(pool.count() == nU
              && pool.data(pool.index(0), MediaPoolModel::IdRole).toULongLong() == idU,
          "undo restaura el medio con su id y posicion");
    undoStack.redo();
    check(pool.count() == nU - 1, "redo vuelve a eliminarlo");
    undoStack.undo();

    const int nbU = pool.bins().size();
    const int binU = pool.addBin(QStringLiteral("Temporal"));
    check(pool.bins().size() == nbU + 1, "addBin (undoable) crea el bin");
    pool.moveToBin(idU, binU);
    check(pool.bins().at(binU).toMap().value("count").toInt() == 1,
          "moveToBin (undoable) asigna el medio");
    undoStack.undo();   // deshace moveToBin
    check(pool.bins().at(binU).toMap().value("count").toInt() == 0,
          "undo deshace la asignacion al bin");
    undoStack.undo();   // deshace addBin
    check(pool.bins().size() == nbU, "undo deshace la creacion del bin");

    qInfo("[POOL-SELFTEST] resultado: %s (%d fallos)", failures ? "FALLO" : "OK", failures);
    return failures ? 1 : 0;
}
