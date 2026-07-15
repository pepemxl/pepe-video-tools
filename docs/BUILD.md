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
  `src/engine/videosurface.*` (**`QQuickItem` con `QSGImageNode`**: sube el fotograma a una
  textura GPU y lo compone con el Scene Graph / RHI), `src/app/videocontroller.*`
  (fachada QML con play/pausa/seek, posición y timecode).
- **Uso:** doble clic en un medio del pool → se abre y reproduce en el monitor **ORIGEN**.
  El botón de reproducción y la barra (clic = buscar) del monitor de origen ya funcionan.
- **Puente decode→RHI:** el fotograma decodificado se sube a `QSGTexture` con
  `QQuickWindow::createTextureFromImage` y se dibuja con `QSGImageNode` (letterbox centrado).
  Ya no se pinta por CPU. Diagnóstico opcional: `PVS_RHI_DEBUG=1` registra por stderr cada
  subida de textura (`[RHI] textura GPU #n subida …`).
- Pendiente: eliminar la copia intermedia `QImage` (hwaccel → textura D3D11 directa),
  `FrameCache`, **audio** (Fase 4) y el compositor multicapa del monitor de **PROGRAMA** (Fase 2).

### Línea de tiempo (Fase 2, en curso)
- `src/app/timelinemodel.*` alimenta las pistas y clips de `TimelinePanel.qml` (singleton
  `PepeVideo.TimelineModel`). Edición no destructiva con **undo/redo** (`QUndoStack`).
- Ya funciona:
  - **Seleccionar** clip (clic) y **cuchilla** (herramienta B → clic sobre el clip).
  - **Arrastrar / mover** clip dentro de la pista y **entre pistas** (arrastre vertical)
    con la herramienta Selección · A. Solo se admite soltar en una pista del mismo tipo
    (audio↔audio, vídeo/título↔vídeo).
  - **Recorte fino / trim** arrastrando los bordes del clip (herramienta Trim · W).
  - **Ripple** (herramienta RR): al recortar la salida, los clips posteriores de la pista se
    desplazan para cerrar/abrir el hueco. Tirador en el borde derecho.
  - **Roll** (herramienta N): arrastra la frontera entre dos clips adyacentes; uno cede lo que
    el otro gana, sin mover el resto ni cambiar la duración total.
  - **Imán (snap) real**: al mover o recortar, el borde se ajusta a bordes de otros clips,
    al playhead y a los marcadores (toggle en la barra o tecla **S**).
  - **Marcadores**: añadir en el playhead (botón de la barra o tecla **M**), eliminar con clic
    sobre el marcador en la regla.
  - **Playhead** por clic en la regla; borrar clip con Supr / Retroceso.
- Atajos: A/T/B/N/Y/W/P/Z (herramientas), S (imán), M (marcador), **Espacio** (reproducir/pausar
  el PROGRAMA), Supr/Retroceso (borrar), Ctrl+Z / Ctrl+Shift+Z (undo/redo).
- **Autotest del modelo** (sin UI): `PVS_TL_SELFTEST=1 ./PepeVideoStudio.exe` valida las
  invariantes de mover/ripple/roll/undo y de `clipsAt` (compositor), impresas por stderr
  (`[TL selftest] … OK/FALLO`).
- Pendiente: **slip/slide** y migración opcional a `QAbstractItemModel`.

### Compositor multicapa · monitor de PROGRAMA (Fase 2, primera etapa)
- En el instante del **playhead**, `TimelineModel::clipsAt()` resuelve el clip de vídeo activo
  en cada pista, ordenados de abajo (V1) a arriba (V3).
- `src/engine/framegrabber.*` decodifica de forma **sincrónica** un fotograma en el tiempo de
  origen de cada clip (seek + decode, RGBA con swscale).
- `src/engine/compositor.*` los **apila** en un fotograma 1280×720 (con `QPainter`) y lo entrega
  por `frameReady(QImage)` a la superficie RHI del monitor de PROGRAMA (singleton
  `PepeVideo.Compositor`). Recompón al mover el playhead o editar la timeline (con antirrebote).
- **Hilo de trabajo:** el decode/composición vive en `CompositorWorker` (un `QThread` propio);
  el `Compositor` (hilo de GUI) le pasa una instantánea de los clips activos por señal encolada
  (`RenderClipList`). Con control **busy/pending** (una composición en vuelo, recomposición al
  día al terminar), así la reproducción **no bloquea la UI** y descarta fotogramas si se retrasa.
- **Reproducción del PROGRAMA:** el `Compositor` tiene un **reloj** (~30 Hz) que avanza el
  playhead en **tiempo real** (con descarte de fotogramas si el decode se retrasa) y reproduce
  la secuencia hasta el final del contenido. Transporte: botón ▶/❚❚ del monitor y tecla
  **Espacio**. Un seek manual durante la reproducción se respeta (el reloj es incremental).
- El scrubber del monitor de PROGRAMA refleja el playhead y permite moverlo (clic).
- **Transform/opacidad por clip:** cada clip lleva una `Transform` (posición, escala, rotación,
  opacidad, recorte) que el worker aplica con `QPainter`. Selecciona un clip en la timeline y
  edítalo en el **Inspector** (sección *Transformar*): arrastra horizontalmente los campos
  Posición/Escala/Rotación y usa el deslizador de opacidad. El monitor de PROGRAMA se recompón
  al instante.
- **Keyframes:** el rombo junto a cada propiedad **añade/quita** un keyframe en el playhead
  (se rellena de ámbar cuando hay uno en el playhead; el borde ámbar indica que la propiedad
  está animada). Con la propiedad animada, arrastrar el valor edita el keyframe del playhead.
  Los keyframes se anclan al **tiempo de origen** del clip (estables al mover/recortar) e
  interpolan linealmente; el compositor los evalúa por fotograma, así que el PROGRAMA **anima**
  al reproducir. Mueve el playhead y los valores del Inspector siguen la animación.
- **Prueba con vídeo real:** `PVS_TL_MEDIA=<ruta>` asigna ese archivo a los clips de V1 al
  arrancar; `PVS_PROGRAM_AUTOPLAY=1` arranca la reproducción del PROGRAMA al cargar. Clips sin
  media se dibujan con su color (para visualizar el apilado).
- **Autotest del compositor** (sin UI): `PVS_COMP_SELFTEST=1 ./PepeVideoStudio.exe` compone
  fotogramas de color en memoria y comprueba opacidad y mezcla de capas por píxel
  (`[COMP selftest] … OK/FALLO`).
- **Transiciones (disolvencia cruzada):** si dos clips de una misma pista **se solapan** en el
  tiempo, la región de solape hace un crossfade — el clip entrante se funde sobre el saliente.
  Se crea arrastrando un clip sobre otro (sin herramienta aparte); la timeline marca el solape
  con un indicador ⤫. El compositor renderiza ambas capas y mezcla por opacidad.
- Pendiente: transiciones explícitas con selector de tipo, y curvas de interpolación
  (bezier/hold) de keyframes.

### Resumen de estado (2026-07-15)
| Fase | Estado |
|------|--------|
| 0 — Shell QML | ✅ completa |
| 1 — Medios y reproducción | 🟢 casi completa (fotograma por GPU/RHI listo; falta audio, hwaccel) |
| 2 — Timeline | ✅ completa (edición + compositor: capas, transform, keyframes, transiciones) |
| 3–6 — Color, audio, títulos, export | ⬜ pendientes |
```
