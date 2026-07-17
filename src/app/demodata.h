#pragma once

// Data de demostración bajo demanda.
//
// La aplicación arranca VACÍA (sin bins, medios ni clips de ejemplo). La mock
// data solo se genera cuando hace falta:
//   - PVS_DEMO=1            → proyecto de demostración completo, usando los
//                             medios reales de LOCAL_DATA/ si existen.
//   - autotests (PVS_*_SELFTEST) que dependen de un proyecto poblado: la
//     siembran justo antes de correr.
//   - hooks de prueba PVS_TL_MEDIA / PVS_TL_AUDIO (asignan media a los clips
//     de demo, así que también la requieren).

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

inline bool pvsEnvSet(const char *name)
{
    return !qEnvironmentVariableIsEmpty(name);
}

// Directorio LOCAL_DATA/ del repo: PVS_LOCAL_DATA lo fija explícitamente; si
// no, se busca subiendo desde el ejecutable (build/mingw → raíz) y desde el
// directorio actual. Vacío si no existe.
inline QString pvsLocalDataDir()
{
    const QString forced = qEnvironmentVariable("PVS_LOCAL_DATA");
    if (!forced.isEmpty() && QDir(forced).exists())
        return QDir(forced).absolutePath();

    const QStringList roots = { QCoreApplication::applicationDirPath(), QDir::currentPath() };
    for (const QString &root : roots) {
        QDir d(root);
        for (int up = 0; up < 6; ++up) {
            if (d.exists(QStringLiteral("LOCAL_DATA")))
                return d.filePath(QStringLiteral("LOCAL_DATA"));
            if (!d.cdUp())
                break;
        }
    }
    return {};
}

// Archivos de medios de LOCAL_DATA/ (ordenados por nombre), filtrados por
// extensión; con la lista vacía devuelve cualquier medio conocido.
inline QStringList pvsLocalDataMedia(QStringList exts = {})
{
    if (exts.isEmpty())
        exts = { QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
                 QStringLiteral("avi"), QStringLiteral("webm"),
                 QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("flac"),
                 QStringLiteral("aac"), QStringLiteral("m4a"),
                 QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg") };
    const QString dir = pvsLocalDataDir();
    if (dir.isEmpty())
        return {};
    QStringList out;
    const QFileInfoList files = QDir(dir).entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files)
        if (exts.contains(fi.suffix().toLower()))
            out.push_back(fi.absoluteFilePath());
    return out;
}

// Primer vídeo de LOCAL_DATA/ (para los clips de demo), o cadena vacía.
inline QString pvsFirstLocalVideo()
{
    const QStringList v = pvsLocalDataMedia({ QStringLiteral("mp4"), QStringLiteral("mov"),
                                              QStringLiteral("mkv"), QStringLiteral("avi"),
                                              QStringLiteral("webm") });
    return v.isEmpty() ? QString() : v.first();
}
