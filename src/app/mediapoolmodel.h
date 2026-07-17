#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QUrl>
#include <QVector>
#include <qqmlintegration.h>

#include <functional>

class QProcess;
class QUndoStack;

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
    int bin = -1;        // índice del bin (carpeta) al que pertenece; -1 = sin bin
};

// Un bin (carpeta) del Media Pool. `parent` = índice del bin padre (-1 = raíz);
// el padre siempre se crea antes que el hijo, así que parent < índice propio.
struct MediaBin {
    QString name;
    QString color;   // color de la etiqueta
    int parent = -1;
};

// Modelo del Media Pool: importa medios reales con ffprobe/ffmpeg.
class MediaPoolModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Filtro de búsqueda (caja "Buscar clips…"): solo se muestran los medios cuyo
    // nombre contiene el texto (sin distinguir mayúsculas). Vacío = todos.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Bins (carpetas): lista {name, color, count, index} y bin activo (-1 = todos).
    // El bin activo filtra la rejilla junto con el texto de búsqueda.
    Q_PROPERTY(QVariantList bins READ bins NOTIFY binsChanged)
    Q_PROPERTY(int currentBin READ currentBin WRITE setCurrentBin NOTIFY currentBinChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectedChanged)
    Q_PROPERTY(QString selectedLine1 READ selectedLine1 NOTIFY selectedChanged)
    Q_PROPERTY(QString selectedLine2 READ selectedLine2 NOTIFY selectedChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole, KindRole, DurationRole, ResolutionRole,
        FpsRole, CodecRole, BitrateRole, ThumbRole, TexRole, InUseRole, IdRole
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

    // --- Bins (carpetas, con anidado de un padre por bin) ---
    // Lista en orden de árbol (DFS) con {index, name, color, count, depth};
    // `count` agrega los medios de los bins descendientes.
    QVariantList bins() const;
    int currentBin() const { return m_currentBin; }
    void setCurrentBin(int bin);
    // Crea un bin (nombre vacío = "Bin N") bajo `parent` (-1 = raíz); devuelve su índice.
    Q_INVOKABLE int addBin(const QString &name, int parent = -1);
    // Renombra un bin (nombre vacío = sin efecto).
    Q_INVOKABLE void renameBin(int index, const QString &name);
    // Elimina un bin: sus medios quedan sin bin, sus hijos suben al padre del
    // eliminado y los índices superiores bajan.
    Q_INVOKABLE void removeBin(int index);
    // Asigna el medio `id` al bin `binIndex` (-1 = quitar del bin).
    Q_INVOKABLE void moveToBin(quint64 id, int binIndex);
    // Elimina un medio del pool. Los clips de la timeline que lo usan se
    // eliminan aparte (TimelineModel::removeClipsWithMedia), previa advertencia
    // en la UI cuando el medio está en uso.
    Q_INVOKABLE void removeItem(quint64 id);

    // Pila de undo compartida (la del proyecto, TimelineModel::undoStack):
    // eliminar medios y las operaciones de bins se vuelven undoables con el
    // Ctrl+Z global. Sin pila (tests unitarios), las operaciones son directas.
    void setUndoStack(QUndoStack *stack) { m_undo = stack; }
    // Persistencia (proyecto): bins como JSON [{name, parent}] (acepta también el
    // formato antiguo de cadenas), bin de una ruta y restauración.
    QJsonArray binsJson() const;
    void setBinsJson(const QJsonArray &arr);
    QStringList binNames() const;
    void setBins(const QStringList &names);
    QString binNameOfPath(const QString &path) const;
    void setPathBin(const QString &path, int binIndex);

signals:
    void countChanged();
    void filterChanged();
    void selectedChanged();
    void mediaImported();      // se añadió un medio (el proyecto se marca sucio)
    void mediaRemoved();       // se quitó un medio (el proyecto se marca sucio)
    void binsChanged();        // cambió la ESTRUCTURA de bins/asignaciones (ensucia)
    void currentBinChanged();  // cambió el bin activo (solo vista, no ensucia)
    void importError(const QString &message);

private:
    void seedDemo();
    void appendItem(const MediaItem &item);
    int rowForId(quint64 id) const;
    // Reconstruye la lista visible según el filtro (reset del modelo).
    void rebuildVisible();
    bool matchesFilter(const MediaItem &it) const;
    // ¿`bin` es `ancestor` o desciende de él?
    bool binIsUnder(int bin, int ancestor) const;
    void generateThumbnail(quint64 id, const QString &path, const QString &kind, double atSeconds);
    // Ejecuta `op` como comando undoable: el undo restaura una instantánea de
    // bins / asignaciones / bin activo. Sin pila, ejecuta `op` directamente.
    void pushPoolOp(const QString &text, std::function<void()> op);

    QVector<MediaItem> m_items;
    QVector<MediaBin> m_bins;
    QVector<int> m_visible;    // filas visibles → índice en m_items
    QString m_filter;
    int m_currentBin = -1;     // bin activo (-1 = todos)
    int m_selected = 0;        // índice del elemento seleccionado en m_items
    quint64 m_nextId = 1;
    QUndoStack *m_undo = nullptr;   // pila compartida del proyecto (puede ser nula)
    QString m_ffprobe;
    QString m_ffmpeg;
    QString m_cacheDir;
};

// Auto-test (PVS_POOL_SELFTEST). Devuelve 0 (OK) / 1 (fallo) / -1 (no solicitado).
int runPoolSelfTestIfRequested();
