# Compilar y ejecutar — PepeVideo Studio

Aplicación de escritorio en **Qt 6 / C++** (UI en Qt Quick/QML). Estas instrucciones son
para Windows con el kit **MinGW** que trae Qt.

---

## 1. Requisitos

| Herramienta | Versión probada | Notas |
|-------------|-----------------|-------|
| Qt 6 (mingw_64) | 6.11.0 | `C:/Qt/6.11.0/mingw_64` |
| MinGW | 13.1.0 | `C:/Qt/Tools/mingw1310_64` |
| CMake | ≥ 3.21 (probado 4.3) | `C:/Qt/Tools/CMake_64` |
| Ninja | — | `C:/Qt/Tools/Ninja` |
| FFmpeg (`ffmpeg`, `ffprobe`) | build reciente | Necesario en el **PATH** para importar medios (Fase 1). |

> Si tus rutas de Qt/MinGW difieren, edita `CMakePresets.json` (sección `configurePresets → cacheVariables`).

---

## 2. Compilar (línea de comandos)

Desde la raíz del repositorio:

```bash
# 1) Configurar (genera build/mingw con el preset MinGW)
cmake --preset mingw

# 2) Compilar
cmake --build build/mingw
```

El ejecutable queda en `build/mingw/PepeVideoStudio.exe`.

Si `cmake`, `ninja` o el compilador no están en el PATH, añádelos temporalmente:

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/Ninja:/c/Qt/6.11.0/mingw_64/bin:$PATH"
```

(En PowerShell: `$env:Path = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\6.11.0\mingw_64\bin;" + $env:Path`)

---

## 3. Ejecutar

```bash
# Necesita las DLL de Qt en el PATH (bin de Qt)
export PATH="/c/Qt/6.11.0/mingw_64/bin:$PATH"
./build/mingw/PepeVideoStudio.exe
```

Desde Qt Creator: abre `CMakeLists.txt` como proyecto, elige el kit *Desktop Qt 6.11.0 MinGW 64-bit* y pulsa **Run** (▶). Qt Creator resuelve las DLL automáticamente.

---

## 4. Distribuir (empaquetar dependencias)

Para ejecutar fuera del entorno de Qt, copia las dependencias junto al `.exe`:

```bash
C:/Qt/6.11.0/mingw_64/bin/windeployqt.exe --qmldir src/qml build/mingw/PepeVideoStudio.exe
```

Esto añade la plataforma `platforms/qwindows.dll`, las DLL de Qt y los módulos QML necesarios.
(Instalador NSIS/Inno Setup: pendiente, Fase 6.)

---

## 5. Solución de problemas

- **La app se cierra al instante sin mensaje.** Es una app GUI (`WIN32_EXECUTABLE`), así que los
  errores de QML NO salen por stderr. Fuérzalos:
  ```bash
  export QT_FORCE_STDERR_LOGGING=1 QT_ASSUME_STDERR_HAS_CONSOLE=1
  ./build/mingw/PepeVideoStudio.exe
  ```
- **`ld returned 1` al compilar.** El `.exe` está bloqueado por una instancia en ejecución.
  Ciérrala: `Get-Process PepeVideoStudio | Stop-Process -Force` (PowerShell).
- **No encuentra el _platform plugin_ (`qwindows.dll`).** Ejecuta desde Qt Creator, o añade el
  `bin` de Qt al PATH, o corre `windeployqt` (paso 4).
- **Importar medios no hace nada.** Asegúrate de que `ffmpeg` y `ffprobe` están en el PATH
  (`ffprobe -version`).

### Convenciones de QML aprendidas (para no repetir errores)
- `font.pixelSize` es **entero**; usa `font.pointSize` si necesitas decimales.
- El color hex con alfa en QML es **`#AARRGGBB`** (¡no CSS `#RRGGBBAA`!). En cambio, dentro de
  `Canvas` (`fillStyle`/`strokeStyle`) se usa sintaxis CSS.
- Los hijos de `RowLayout`/`ColumnLayout` usan `Layout.alignment`, no `anchors.*`.

---

## 6. Maqueta de referencia (prototipo Electron)

El diseño original está reproducido en HTML en `design-reference/` (solo referencia visual):

```bash
cd design-reference
npm install
npm start
```

---

## 7. Medios y motor de vídeo (estado)

### Importar medios (Fase 1)
- Botón **`+`** del Media Pool → abre el diálogo **nativo** de Windows (sin dependencias QML extra).
- Metadatos con `ffprobe`, miniaturas/formas de onda con `ffmpeg` (deben estar en el PATH).
- Variables de entorno útiles:
  - `PVS_DEMO_MEDIA=<ruta>` — importa ese archivo al arrancar (para pruebas).
  - `PVS_FFMPEG` / `PVS_FFPROBE` — rutas a los ejecutables si no están en el PATH.
- Miniaturas cacheadas en `%LOCALAPPDATA%/Pepe/PepeVideo Studio/cache/thumbs`.

### Reproducción de vídeo (Fase 1)
- **Decodificación real con libav\*** enlazada desde `C:/FFMPEG` (build shared/dev). Se configura
  con la variable de CMake `FFMPEG_ROOT` (por defecto `C:/FFMPEG`):
  ```bash
  cmake --preset mingw -DFFMPEG_ROOT=C:/FFMPEG
  ```
- Las DLL de FFmpeg (`avcodec-62`, `avformat-62`, `avutil-60`, `swscale-9`, `swresample-6`, …) se
  **copian automáticamente** junto al `.exe` tras compilar (paso `POST_BUILD` del CMake).
- Componentes: `src/engine/videodecoder.*` (demux/decode/swscale en hilo de trabajo),
  `src/engine/videosurface.*` (`QQuickPaintedItem` que pinta el fotograma), `src/app/videocontroller.*`
  (fachada QML con play/pausa/seek, posición y timecode).
- **Uso:** doble clic en un medio del pool → se abre y reproduce en el monitor **ORIGEN**.
  El botón de reproducción y la barra (clic = buscar) del monitor de origen ya funcionan.
- Pendiente: subir el fotograma a **textura RHI/QSG** (ahora se pinta por CPU), y **audio**
  (Fase 4). El compositor multicapa del monitor de **PROGRAMA** es de la Fase 2.

### Línea de tiempo (Fase 2, en curso)
- `src/app/timelinemodel.*` alimenta las pistas y clips de `TimelinePanel.qml` (singleton
  `PepeVideo.TimelineModel`). Edición no destructiva con **undo/redo** (`QUndoStack`).
- Ya funciona:
  - **Seleccionar** clip (clic) y **cuchilla** (herramienta B → clic sobre el clip).
  - **Arrastrar / mover** clip dentro de la pista (herramienta Selección · A).
  - **Recorte fino / trim** arrastrando los bordes del clip (herramienta Trim · W); aparecen
    tiradores en los extremos.
  - **Imán (snap) real**: al mover o recortar, el borde se ajusta a bordes de otros clips,
    al playhead y a los marcadores (toggle en la barra o tecla **S**).
  - **Marcadores**: añadir en el playhead (botón de la barra o tecla **M**), eliminar con clic
    sobre el marcador en la regla.
  - **Playhead** por clic en la regla; borrar clip con Supr / Retroceso.
- Atajos: A/T/B/N/Y/W/P/Z (herramientas), S (imán), M (marcador), Supr/Retroceso (borrar),
  Ctrl+Z / Ctrl+Shift+Z (undo/redo).
- Pendiente: arrastre **entre pistas** (vertical), **ripple**/**roll**, y el **compositor
  multicapa** hacia el monitor de PROGRAMA.

### Resumen de estado (2026-07-15)
| Fase | Estado |
|------|--------|
| 0 — Shell QML | ✅ completa |
| 1 — Medios y reproducción | 🟢 casi completa (falta audio y textura RHI) |
| 2 — Timeline | 🟡 en curso (undo/redo, cuchilla, mover, trim, snap, marcadores) |
| 3–6 — Color, audio, títulos, export | ⬜ pendientes |
```
