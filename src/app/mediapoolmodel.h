#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QUrl>
#include <QVector>
#include <qqmlintegration.h>

class QProcess;

// Un elemento de medios importado o de demostración.
struct MediaItem {
    quint64 id = 0;
    QString name;
    QString path;
    QString kind;        // "video" | "audio" | "image"
    QString duration;    // mm:ss
    QString resolution;  // "3840×2160" (vacío para audio)
    QString fps;         // "29.97"
    QString codec;       // "H.265"
    QString bitrate;     // "148 Mb/s"
    QString pixFmt;      // "yuv420p10le"
    QString thumb;       // ruta de miniatura (vacío mientras se genera)
    QString tex;         // color placeholder cuando no hay miniatura
    bool inUse = false;
};

// Modelo del Media Pool: importa medios reales con ffprobe/ffmpeg.
class MediaPoolModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectedChanged)
    Q_PROPERTY(QString selectedLine1 READ selectedLine1 NOTIFY selectedChanged)
    Q_PROPERTY(QString selectedLine2 READ selectedLine2 NOTIFY selectedChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole, KindRole, DurationRole, ResolutionRole,
        FpsRole, CodecRole, BitrateRole, ThumbRole, TexRole, InUseRole
    };

    explicit MediaPoolModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_items.size()); }
    int selectedIndex() const { return m_selected; }
    void setSelectedIndex(int i);
    QString selectedName() const;
    QString selectedLine1() const;
    QString selectedLine2() const;

    // Abre el diálogo nativo de selección de archivos e importa lo elegido.
    Q_INVOKABLE void openImportDialog();
    // Importa desde una URL de archivo o desde una ruta directa.
    Q_INVOKABLE void importFile(const QUrl &url);
    Q_INVOKABLE void importPath(const QString &path);

signals:
    void countChanged();
    void selectedChanged();
    void importError(const QString &message);

private:
    void seedDemo();
    void appendItem(const MediaItem &item);
    int rowForId(quint64 id) const;
    void generateThumbnail(quint64 id, const QString &path, const QString &kind, double atSeconds);

    QVector<MediaItem> m_items;
    int m_selected = 0;
    quint64 m_nextId = 1;
    QString m_ffprobe;
    QString m_ffmpeg;
    QString m_cacheDir;
};
