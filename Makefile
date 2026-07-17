# ============================================================================
#  PepeVideo Studio — Makefile de conveniencia para Windows (MinGW + Qt6)
# ----------------------------------------------------------------------------
#  Envuelve el flujo CMake + Ninja. Pensado para `mingw32-make` (incluido en
#  las Tools de Qt) o cualquier GNU make en Windows.
#
#  Uso rapido:
#     mingw32-make            # configura (si hace falta) y compila
#     mingw32-make run        # ejecuta la app
#     mingw32-make selftest   # auto-tests deterministicos (audio)
#     mingw32-make deploy     # copia DLLs/plugins de Qt (windeployqt)
#     mingw32-make clean      # borra objetos; distclean borra todo el build
#     mingw32-make help       # lista de objetivos
#
#  Sobrescribe rutas si tu instalacion difiere, p. ej.:
#     mingw32-make QT_DIR=C:/Qt/6.11.0/mingw_64 FFMPEG_ROOT=C:/FFMPEG build
# ============================================================================

# En Windows fuerza cmd.exe como shell de las recetas (para que `set VAR=`, el PATH
# con `;` y las rutas con `\` se interpreten correctamente aunque `make` se invoque
# desde un entorno con `sh` en el PATH, p. ej. Git Bash).
ifeq ($(OS),Windows_NT)
  SHELL := cmd.exe
  .SHELLFLAGS := /C
endif

# ---- Rutas configurables (ajusta a tu maquina o pasalas por linea de comandos) ----
QT_DIR      ?= C:/Qt/6.11.0/mingw_64
MINGW_DIR   ?= C:/Qt/Tools/mingw1310_64
NINJA_DIR   ?= C:/Qt/Tools/Ninja
FFMPEG_ROOT ?= C:/FFMPEG
BUILD_DIR   ?= build/mingw
CONFIG      ?= Debug

CMAKE  = cmake
EXE     = $(BUILD_DIR)/PepeVideoStudio.exe
EXE_WIN = $(subst /,\,$(EXE))
CACHE   = $(BUILD_DIR)/CMakeCache.txt

# Qt, el compilador y Ninja en el PATH (para configurar, compilar y cargar las DLLs).
# Al fijar QT_PLUGIN_PATH hay que fijar TAMBIEN QML_IMPORT_PATH, o Qt deja de
# autodetectar los modulos QML (QtQuick.Controls) y los menus no cargarian.
export PATH := $(QT_DIR)/bin;$(MINGW_DIR)/bin;$(NINJA_DIR);$(PATH)
export QT_PLUGIN_PATH := $(QT_DIR)/plugins
export QML_IMPORT_PATH := $(QT_DIR)/qml
export QML2_IMPORT_PATH := $(QT_DIR)/qml

.PHONY: all help configure build rebuild run run-wait clean distclean deploy installer \
        selftest selftest-audio selftest-comp selftest-tl selftest-export selftest-wave \
        selftest-proj selftest-pool

all: build

## help: muestra esta ayuda
help:
	@echo PepeVideo Studio - objetivos disponibles:
	@echo   configure   Genera el proyecto (CMake + Ninja)
	@echo   build       Compila (configura si hace falta)  [por defecto]
	@echo   rebuild     Recompila desde cero
	@echo   run         Ejecuta la aplicacion
	@echo   selftest    Auto-test de audio (deterministico, sin ventana)
	@echo   selftest-wave  Auto-test de formas de onda (deterministico, sin ventana)
	@echo   selftest-proj  Auto-test de proyecto guardar/abrir (deterministico, sin ventana)
	@echo   selftest-pool  Auto-test del Media Pool: filtro y bins (deterministico, sin ventana)
	@echo   deploy      Copia DLLs/plugins de Qt junto al .exe (windeployqt)
	@echo   clean       Borra los objetos de compilacion
	@echo   distclean   Borra por completo el directorio de build
	@echo.
	@echo Rutas: QT_DIR=$(QT_DIR)  FFMPEG_ROOT=$(FFMPEG_ROOT)  BUILD_DIR=$(BUILD_DIR)

## configure: genera el proyecto con el generador Ninja
configure $(CACHE):
	$(CMAKE) -S . -B $(BUILD_DIR) -G Ninja \
	  -DCMAKE_BUILD_TYPE=$(CONFIG) \
	  -DCMAKE_PREFIX_PATH="$(QT_DIR)" \
	  -DFFMPEG_ROOT="$(FFMPEG_ROOT)" \
	  -DCMAKE_C_COMPILER="$(MINGW_DIR)/bin/gcc.exe" \
	  -DCMAKE_CXX_COMPILER="$(MINGW_DIR)/bin/g++.exe"

## build: compila (configura automaticamente si no hay cache)
build: $(CACHE)
	$(CMAKE) --build $(BUILD_DIR)

## rebuild: limpia y recompila
rebuild: clean build

## run: ejecuta la aplicacion (desacoplada; la terminal no queda bloqueada)
run: build
	start "PepeVideo Studio" "$(EXE_WIN)"

## run-wait: ejecuta la app en primer plano (bloquea hasta cerrarla)
run-wait: build
	"$(EXE_WIN)"

## selftest: auto-test de audio (decodificacion/mezcla/LUFS); termina solo
# QT_QPA_FONTDIR apunta la base de datos de fuentes del backend offscreen a las
# fuentes de Windows (Qt ya no incluye fuentes propias) para evitar el aviso
# "QFontDatabase: Cannot find font directory".
selftest selftest-audio: build
	set "PVS_AUDIO_SELFTEST=1" && set "QT_QPA_PLATFORM=offscreen" && set "QT_QPA_FONTDIR=C:\Windows\Fonts" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## selftest-export: compone y codifica ~2 s a un MP4 temporal (H.264/AAC); termina solo
selftest-export: build
	set "PVS_EXPORT_SELFTEST=1" && set "QT_QPA_PLATFORM=offscreen" && set "QT_QPA_FONTDIR=C:\Windows\Fonts" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## selftest-wave: envolvente de PCM real (formas de onda del timeline); termina solo
selftest-wave: build
	set "PVS_WAVE_SELFTEST=1" && set "QT_QPA_PLATFORM=offscreen" && set "QT_QPA_FONTDIR=C:\Windows\Fonts" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## selftest-proj: guardar/abrir proyecto (round-trip, dirty); termina solo
selftest-proj: build
	set "PVS_PROJ_SELFTEST=1" && set "QT_QPA_PLATFORM=offscreen" && set "QT_QPA_FONTDIR=C:\Windows\Fonts" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## selftest-pool: filtro y bins del Media Pool; termina solo
selftest-pool: build
	set "PVS_POOL_SELFTEST=1" && set "QT_QPA_PLATFORM=offscreen" && set "QT_QPA_FONTDIR=C:\Windows\Fonts" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## selftest-comp: auto-test del compositor (abre la app; cierrala para terminar)
selftest-comp: build
	set "PVS_COMP_SELFTEST=1" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## selftest-tl: auto-test del modelo de linea de tiempo (abre la app)
selftest-tl: build
	set "PVS_TL_SELFTEST=1" && set "QT_FORCE_STDERR_LOGGING=1" && "$(EXE_WIN)"

## deploy: empaqueta DLLs y plugins de Qt junto al ejecutable (build redistribuible)
deploy: build
	"$(QT_DIR)/bin/windeployqt.exe" --qmldir src/qml "$(EXE_WIN)"

## installer: prepara build/…/installer-stage y compila el setup NSIS (requiere makensis)
installer: build
	$(CMAKE) -E rm -rf "$(BUILD_DIR)/installer-stage"
	$(CMAKE) -E make_directory "$(BUILD_DIR)/installer-stage"
	$(CMAKE) -E copy "$(EXE_WIN)" "$(BUILD_DIR)/installer-stage"
	-cmd /c copy /Y "$(subst /,\,$(BUILD_DIR))\*.dll" "$(subst /,\,$(BUILD_DIR))\installer-stage\" >nul
	"$(QT_DIR)/bin/windeployqt.exe" --qmldir src/qml --dir "$(BUILD_DIR)/installer-stage" "$(EXE_WIN)"
	-makensis "/DSTAGE=..\$(subst /,\,$(BUILD_DIR))\installer-stage" installer\PepeVideoStudio.nsi

## clean: borra los artefactos de compilacion (conserva la cache)
clean:
	-$(CMAKE) --build $(BUILD_DIR) --target clean

## distclean: elimina todo el directorio de build
distclean:
	-$(CMAKE) -E rm -rf $(BUILD_DIR)
