@echo off
rem ============================================================================
rem  PepeVideo Studio - make.bat (Windows, sin necesidad de GNU make)
rem  Envuelve el flujo CMake + Ninja. Uso:
rem     make               configura (si hace falta) y compila
rem     make run           ejecuta la aplicacion
rem     make selftest      auto-test de audio (deterministico, sin ventana)
rem     make deploy        copia DLLs/plugins de Qt (windeployqt)
rem     make clean         borra objetos ; make distclean borra todo el build
rem     make help          lista de objetivos
rem  Sobrescribe rutas exportando variables antes de llamar, p. ej.:
rem     set QT_DIR=C:/Qt/6.11.0/mingw_64
rem     make build
rem ============================================================================
setlocal EnableExtensions

rem ---- Rutas configurables (usa las del entorno si ya estan definidas) ----
if not defined QT_DIR      set "QT_DIR=C:/Qt/6.11.0/mingw_64"
if not defined MINGW_DIR   set "MINGW_DIR=C:/Qt/Tools/mingw1310_64"
if not defined NINJA_DIR   set "NINJA_DIR=C:/Qt/Tools/Ninja"
if not defined FFMPEG_ROOT set "FFMPEG_ROOT=C:/FFMPEG"
if not defined BUILD_DIR   set "BUILD_DIR=build/mingw"
if not defined CONFIG      set "CONFIG=Debug"

rem Qt, el compilador y Ninja deben estar en el PATH para configurar/compilar y
rem para que la app cargue las DLLs de Qt en tiempo de ejecucion.
set "PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%NINJA_DIR%;%PATH%"
set "QT_PLUGIN_PATH=%QT_DIR%\plugins"

rem Ruta del ejecutable con barras invertidas (para poder lanzarlo desde cmd).
set "EXE=%BUILD_DIR:/=\%\PepeVideoStudio.exe"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=build"

if /I "%TARGET%"=="help"           goto :help
if /I "%TARGET%"=="configure"      goto :configure
if /I "%TARGET%"=="build"          goto :build
if /I "%TARGET%"=="rebuild"        goto :rebuild
if /I "%TARGET%"=="run"            goto :run
if /I "%TARGET%"=="selftest"       goto :selftest
if /I "%TARGET%"=="selftest-audio" goto :selftest
if /I "%TARGET%"=="selftest-comp"  goto :selftest_comp
if /I "%TARGET%"=="selftest-tl"    goto :selftest_tl
if /I "%TARGET%"=="deploy"         goto :deploy
if /I "%TARGET%"=="clean"          goto :clean
if /I "%TARGET%"=="distclean"      goto :distclean

echo Objetivo desconocido: %TARGET%
echo Ejecuta "make help" para ver los objetivos disponibles.
exit /b 1

rem ============================ Objetivos ============================

:help
echo PepeVideo Studio - objetivos disponibles:
echo    configure   Genera el proyecto (CMake + Ninja)
echo    build       Compila (configura si hace falta)  [por defecto]
echo    rebuild     Recompila desde cero
echo    run         Ejecuta la aplicacion
echo    selftest    Auto-test de audio (deterministico, sin ventana)
echo    selftest-comp / selftest-tl   Auto-tests que abren la app (cierrala para terminar)
echo    deploy      Copia DLLs/plugins de Qt junto al .exe (windeployqt)
echo    clean       Borra los objetos de compilacion
echo    distclean   Borra por completo el directorio de build
echo.
echo Rutas:  QT_DIR=%QT_DIR%  FFMPEG_ROOT=%FFMPEG_ROOT%  BUILD_DIR=%BUILD_DIR%
goto :end

:configure
call :do_configure
goto :end

:build
call :do_build
goto :end

:rebuild
call :do_clean
call :do_build
goto :end

:run
call :do_build || goto :fail
"%EXE%"
goto :end

:selftest
call :do_build || goto :fail
set "PVS_AUDIO_SELFTEST=1"
set "QT_QPA_PLATFORM=offscreen"
set "QT_FORCE_STDERR_LOGGING=1"
"%EXE%"
goto :end

:selftest_comp
call :do_build || goto :fail
set "PVS_COMP_SELFTEST=1"
set "QT_FORCE_STDERR_LOGGING=1"
"%EXE%"
goto :end

:selftest_tl
call :do_build || goto :fail
set "PVS_TL_SELFTEST=1"
set "QT_FORCE_STDERR_LOGGING=1"
"%EXE%"
goto :end

:deploy
call :do_build || goto :fail
"%QT_DIR%\bin\windeployqt.exe" --qmldir src\qml "%EXE%"
goto :end

:clean
cmake --build "%BUILD_DIR%" --target clean
goto :end

:distclean
cmake -E rm -rf "%BUILD_DIR%"
goto :end

rem ============================ Subrutinas ============================

:do_configure
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_PREFIX_PATH="%QT_DIR%" -DFFMPEG_ROOT="%FFMPEG_ROOT%" -DCMAKE_C_COMPILER="%MINGW_DIR%/bin/gcc.exe" -DCMAKE_CXX_COMPILER="%MINGW_DIR%/bin/g++.exe"
goto :eof

:do_build
if not exist "%BUILD_DIR%\CMakeCache.txt" call :do_configure
cmake --build "%BUILD_DIR%"
goto :eof

:do_clean
cmake --build "%BUILD_DIR%" --target clean
goto :eof

rem ============================ Salida ============================

:end
exit /b %ERRORLEVEL%

:fail
exit /b 1
