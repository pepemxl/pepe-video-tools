#include "mediapoolmodel.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

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

    seedDemo();

    // Importación automática de demostración (para pruebas): PVS_DEMO_MEDIA=ruta
    const QString demo = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PVS_DEMO_MEDIA"));
    if (!demo.isEmpty())
        importPath(demo);
}

void MediaPoolModel::seedDemo()
{
    // Elementos de muestra (sin miniatura real) para no arrancar vacío.
    const struct { const char *name; const char *kind; const char *dur; const char *res;
                   const char *fps; const char *codec; const char *br; const char *tex; bool used; } demo[] = {
        {"A017_mercado",    "video", "00:14", "3840×2160", "29.97", "H.265", "132 Mb/s", "#26303a", false},
        {"A012_entrevista", "video", "02:41", "3840×2160", "29.97", "H.265", "128 Mb/s", "#2a2632", false},
        {"B009_puesto",     "video", "00:22", "3840×2160", "29.97", "H.265", "148 Mb/s", "#26303a", true},
        {"Drone_zocalo",    "video", "01:08", "3840×2160", "29.97", "H.265", "120 Mb/s", "#233028", false},
        {"A020_calle",      "video", "00:37", "3840×2160", "29.97", "H.265", "130 Mb/s", "#26303a", false},
        {"Musica_intro",    "audio", "03:12", "",          "",      "WAV",   "1536 kb/s","#2b2632", false},
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
        m_items.push_back(it);
    }
}

int MediaPoolModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_items.size());
}

QVariant MediaPoolModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const MediaItem &it = m_items.at(index.row());
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
    default:             return {};
    }
}

QHash<int, QByteArray> MediaPoolModel::roleNames() const
{
    return {
        {NameRole, "nm"}, {PathRole, "path"}, {KindRole, "kind"},
        {DurationRole, "dur"}, {ResolutionRole, "res"}, {FpsRole, "fps"},
        {CodecRole, "codec"}, {BitrateRole, "bitrate"}, {ThumbRole, "thumb"},
        {TexRole, "tex"}, {InUseRole, "used"}
    };
}

void MediaPoolModel::setSelectedIndex(int i)
{
    if (i == m_selected)
        return;
    m_selected = i;
    emit selectedChanged();
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
    beginInsertRows({}, int(m_items.size()), int(m_items.size()));
    m_items.push_back(item);
    endInsertRows();
    emit countChanged();
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
        const QModelIndex idx = index(row);
        emit dataChanged(idx, idx, {ThumbRole});
        if (row == m_selected)
            emit selectedChanged();
    });
    proc->start(m_ffmpeg, args);
}
