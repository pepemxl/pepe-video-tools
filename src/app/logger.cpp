#include "logger.h"
#include "demodata.h"   // pvsLocalDataDir()

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>

#include <cstdio>
#include <cstdlib>

namespace {

QFile *g_logFile = nullptr;
QMutex g_logMutex;                    // el worker de exportación también registra
QtMessageHandler g_prevHandler = nullptr;
QString g_logPath;

const char *levelStr(QtMsgType t)
{
    switch (t) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO ";
    case QtWarningMsg:  return "WARN ";
    case QtCriticalMsg: return "CRIT ";
    case QtFatalMsg:    return "FATAL";
    }
    return "?????";
}

void logHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    {
        QMutexLocker lock(&g_logMutex);
        if (g_logFile && g_logFile->isOpen()) {
            const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
            const QByteArray line = QStringLiteral("%1 [%2] %3\n")
                                        .arg(ts, QLatin1String(levelStr(type)), msg).toUtf8();
            g_logFile->write(line);
            g_logFile->flush();   // crítico: sobrevive a un crash posterior
        }
    }
    // Reenvía al handler previo (stderr en desarrollo).
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
    else {
        fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
        fflush(stderr);
    }
    if (type == QtFatalMsg)
        abort();
}

} // namespace

QString pvsInstallLogger()
{
    // Directorio base: LOCAL_DATA del repo; si no se encuentra, ./LOCAL_DATA.
    QString base = pvsLocalDataDir();
    if (base.isEmpty())
        base = QDir::currentPath() + QStringLiteral("/LOCAL_DATA");

    const QString logsDir = base + QStringLiteral("/LOGS");
    QDir().mkpath(logsDir);

    const QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))
                         + QStringLiteral(".log");
    g_logPath = QDir(logsDir).filePath(name);

    g_logFile = new QFile(g_logPath);
    if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        delete g_logFile;
        g_logFile = nullptr;
        g_logPath.clear();
        return {};
    }

    g_prevHandler = qInstallMessageHandler(logHandler);
    return g_logPath;
}

QString pvsLogFilePath()
{
    return g_logPath;
}

void pvsLogFlush()
{
    QMutexLocker lock(&g_logMutex);
    if (g_logFile)
        g_logFile->flush();
}
