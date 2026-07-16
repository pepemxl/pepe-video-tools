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
    // Filtro de búsqueda (caja "Buscar clips…"): solo se muestran los medios cuyo
    // nombre contiene el texto (sin distinguir mayúsculas). Vacío = todos.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
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

    // Nº de medios VISIBLES (tras el filtro); rowCount() coincide.
    int count() const { return int(m_visible.size()); }
    QString filter() const { return m_filter; }
    void setFilter(const QString &f);
    // selectedIndex es la FILA VISIBLE (-1 si el seleccionado quedó filtrado);
    // internamente la selección se guarda como índice del elemento (estable).
    int selectedIndex() const { return int(m_visible.indexOf(m_selected)); }
    void setSelectedIndex(int i);
    QString selectedName() const;
    QString selectedLine1() const;
    QString selectedLine2() const;

    // Abre el diálogo nativo de selección de archivos e importa lo elegido.
    Q_INVOKABLE void openImportDialog();
    // Importa desde una URL de archivo o desde una ruta directa.
    Q_INVOKABLE void importFile(const QUrl &url);
    Q_INVOKABLE void importPath(const QString &path);

    // Rutas de los medios reales importados (para guardarlas en el proyecto).
    QStringList mediaPaths() const;
    bool containsPath(const QString &path) const;

signals:
    void countChanged();
    void filterChanged();
    void selectedChanged();
    void mediaImported();      // se añadió un medio (el proyecto se marca sucio)
    void importError(const QString &message);

private:
    void seedDemo();
    void appendItem(const MediaItem &item);
    int rowForId(quint64 id) const;
    // Reconstruye la lista visible según el filtro (reset del modelo).
    void rebuildVisible();
    bool matchesFilter(const MediaItem &it) const;
    void generateThumbnail(quint64 id, const QString &path, const QString &kind, double atSeconds);

    QVector<MediaItem> m_items;
    QVector<int> m_visible;    // filas visibles → índice en m_items
    QString m_filter;
    int m_selected = 0;        // índice del elemento seleccionado en m_items
    quint64 m_nextId = 1;
    QString m_ffprobe;
    QString m_ffmpeg;
    QString m_cacheDir;
};
