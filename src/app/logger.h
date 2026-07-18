#pragma once

#include <QString>

// Sistema de logs de la aplicación. Instala un manejador de mensajes de Qt que vuelca
// todos los qDebug/qInfo/qWarning/qCritical/qFatal a
// LOCAL_DATA/LOGS/<YYYYMMDD>_<HHMMSS>.log con marca de tiempo y nivel, haciendo flush
// inmediato para que el registro sobreviva a un crash. Devuelve la ruta del log (o "").
QString pvsInstallLogger();

// Ruta del archivo de log activo ("" si no se instaló).
QString pvsLogFilePath();

// Fuerza el volcado del búfer a disco (por si acaso).
void pvsLogFlush();
