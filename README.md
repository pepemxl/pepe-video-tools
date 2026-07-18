# PepeVideo Studio

Editor de video profesional de escritorio para Windows (con vista a macOS/Linux), construido
en **Qt 6 / C++20**: UI en **Qt Quick (QML)** y **motor de edición propio sobre FFmpeg (libav\*)**
con composición por GPU vía **Qt RHI**. Interfaz en **español**.

No es un "cortar y unir": es una herramienta de nivel de producción que **organiza, corrige,
sincroniza, pule y entrega** el material con calidad final, con edición **no destructiva** y
undo/redo completo.

---

## Características principales

- **Media Pool** con importación real (metadatos `ffprobe`, miniaturas `ffmpeg`), búsqueda,
  bins anidados con arrastrar-y-soltar y persistencia en el proyecto.
- **Monitores dobles** ORIGEN / PROGRAMA con reproducción real (decode libav\*, hwaccel
  D3D11VA, ruta zero-copy a GPU), zoom/paneo del visor y guardado de fotograma a PNG.
- **Línea de tiempo multipista** (V3–V1 / A1–A3): selección, cuchilla, mover entre pistas,
  trim, ripple (ambos bordes), roll, slide, borrado con ripple, imán (snap) real, marcadores
  y zoom.
- **Transiciones editables** por solape: disolvencia cruzada, fundido por negro y barrido.
- **Inspector**: transformación (posición/escala/rotación/opacidad) con **keyframes** e
  interpolación L/H/S, editor de curvas, remapeo de velocidad y manipulador on-screen en el
  monitor de PROGRAMA.
- **Corrección de color primaria** por clip (ruedas lift/gamma/gain + temp/tinte/saturación,
  animable) con **scopes reales**: histograma RGB, waveform y vectorscopio.
- **Audio**: mezcla multi-clip con ganancia/pan automatizables, mute/solo por pista,
  formas de onda reales en la timeline, medidores A1–A3/MAIN y LUFS (integrado y momentáneo).
- **Títulos y lower thirds** renderizados en el compositor; **subtítulos .srt** con
  import/export nativo y render del cue activo.
- **Proyectos `.pvsproj`** (JSON) con guardar/abrir, autoguardado e indicador de cambios.
- **Exportación** MP4 (H.264/AAC) con presets (YouTube, Vimeo…), resolución, fps y bitrate
  configurables, con progreso en la barra de estado.

El estado detallado por fases está en [`PLAN/plan_app.md`](PLAN/plan_app.md) y en el resumen
al final de [`docs/BUILD.md`](docs/BUILD.md).

## Stack

| Capa | Tecnología |
|------|-----------|
| UI | Qt Quick / QML (Qt 6.11), tema oscuro/ámbar propio |
| Lógica y modelos | C++20 expuesto a QML (`Q_PROPERTY`, `Q_INVOKABLE`, `QUndoStack`) |
| Decode / export | FFmpeg `libavformat` / `libavcodec` / `libswscale` / `libswresample` |
| Composición / preview | Qt RHI (D3D11), shaders YUV→RGB + gradación en GPU |
| Audio | Qt Multimedia (`QAudioSink`) + grafo de mezcla propio |
| Build | CMake + Ninja (kit MinGW), `make.bat` / `Makefile` de conveniencia |

## Compilar y ejecutar

Requisitos: Qt 6.11 (mingw_64), MinGW 13.1, CMake ≥ 3.21, Ninja y FFmpeg (`ffmpeg`/`ffprobe`
en el PATH). Instrucciones completas en [`docs/BUILD.md`](docs/BUILD.md).

```bash
cmake --preset mingw          # configurar (build/mingw)
cmake --build build/mingw     # compilar
./build/mingw/PepeVideoStudio.exe
```

O con los scripts de conveniencia (Windows):

```bat
make build     :: compila (configura si hace falta)
make run       :: compila y ejecuta
make selftest  :: auto-tests deterministas sin ventana
make installer :: staging + instalador NSIS
```

## Documentación

- [`docs/user_tutorial.md`](docs/user_tutorial.md) — **tutorial paso a paso** con capturas de pantalla.
- [`docs/instructions.md`](docs/instructions.md) — **manual de uso** de la aplicación.
- [`docs/BUILD.md`](docs/BUILD.md) — compilar, ejecutar, distribuir y solución de problemas.
- [`PLAN/plan_app.md`](PLAN/plan_app.md) — plan de producto, arquitectura y roadmap.

## Estructura del repositorio

```
pepe-video-tools/
├── CMakeLists.txt / CMakePresets.json / Makefile / make.bat
├── PLAN/plan_app.md       # plan y roadmap
├── docs/                  # BUILD.md, instructions.md
├── installer/             # instalador NSIS
├── src/
│   ├── main.cpp           # registro de singletons QML
│   ├── app/               # modelos C++ (timeline, media pool, proyecto, export…)
│   ├── engine/            # motor: decode, compositor, audio, scopes, superficies RHI
│   ├── qml/               # Main.qml, components/, panels/, theme/
│   └── shaders/           # shaders .frag/.vert (YUV, gradación)
└── design-reference/      # maqueta HTML/Electron del diseño (solo referencia visual)
```
