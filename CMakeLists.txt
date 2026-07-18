cmake_minimum_required(VERSION 3.21)

project(PepeVideoStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick Gui GuiPrivate Multimedia ShaderTools)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(PepeVideoStudio
    src/main.cpp
    src/app/theme.h
    src/app/demodata.h
    src/app/logger.h
    src/app/logger.cpp
    src/app/mediapoolmodel.h
    src/app/mediapoolmodel.cpp
    src/app/timelinemodel.h
    src/app/timelinemodel.cpp
    src/app/projectmodel.h
    src/app/projectmodel.cpp
    src/app/videocontroller.h
    src/app/videocontroller.cpp
    src/engine/videodecoder.h
    src/engine/videodecoder.cpp
    src/engine/videoframe.h
    src/engine/videosurface.h
    src/engine/videosurface.cpp
    src/engine/yuvmaterial.h
    src/engine/yuvmaterial.cpp
    src/engine/gradematerial.h
    src/engine/gradematerial.cpp
    src/engine/framegrabber.h
    src/engine/framegrabber.cpp
    src/engine/compositor.h
    src/engine/compositor.cpp
    src/engine/scopesprovider.h
    src/engine/scopesprovider.cpp
    src/engine/scopeview.h
    src/engine/scopeview.cpp
    src/engine/audioengine.h
    src/engine/audioengine.cpp
    src/engine/exporter.h
    src/engine/exporter.cpp
    src/engine/waveformprovider.h
    src/engine/waveformprovider.cpp
)

qt_add_qml_module(PepeVideoStudio
    URI PepeVideo
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
        src/qml/components/TitleBar.qml
        src/qml/components/TopBar.qml
        src/qml/components/MenuButton.qml
        src/qml/components/StatusBar.qml
        src/qml/components/Tip.qml
        src/qml/panels/MediaPool.qml
        src/qml/panels/MonitorsRow.qml
        src/qml/panels/Inspector.qml
        src/qml/panels/TimelinePanel.qml
        src/qml/panels/DeliverPanel.qml
        src/qml/panels/FusionPanel.qml
        src/qml/panels/AudioMixerPanel.qml
)

# `Theme` es un singleton de C++ (src/app/theme.h) registrado en main.cpp.

# Shaders del material YUV (conversión YUV→RGB en GPU), compilados a .qsb con qsb.
# BATCHABLE: genera la variante de vertex shader que exige el batch renderer de
# Qt Quick (sin ella el backend D3D11 no encuentra HLSL utilizable).
qt_add_shaders(PepeVideoStudio "pvs_shaders"
    BATCHABLE
    PREFIX "/pvs"
    BASE "src"
    FILES
        src/shaders/yuv.vert
        src/shaders/yuv.frag
        src/shaders/grade.vert
        src/shaders/grade.frag
        src/shaders/gradeyuv.vert
        src/shaders/gradeyuv.frag
)

# GuiPrivate: da acceso a los headers de QRhi (<rhi/qrhi.h>), la API "semi-pública"
# que usa la ruta zero-copy de la VideoSurface (envolver texturas D3D11 nativas).
target_link_libraries(PepeVideoStudio PRIVATE Qt6::Quick Qt6::Gui Qt6::GuiPrivate Qt6::Multimedia)

# Diálogo nativo de archivos (GetOpenFileNameW) y GUIDs de D3D (IID_ID3D10Multithread,
# para la protección multihilo del dispositivo compartido en el decode zero-copy).
if (WIN32)
    target_link_libraries(PepeVideoStudio PRIVATE comdlg32 dxguid)
endif()

# ===== FFmpeg (build shared/dev en C:/FFMPEG) =====
set(FFMPEG_ROOT "C:/FFMPEG" CACHE PATH "FFmpeg shared/dev build (include/ lib/ bin/)")
target_include_directories(PepeVideoStudio PRIVATE "${FFMPEG_ROOT}/include")
target_link_libraries(PepeVideoStudio PRIVATE
    "${FFMPEG_ROOT}/lib/avformat.lib"
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swscale.lib"
    "${FFMPEG_ROOT}/lib/swresample.lib"
)

# Copiar las DLL de FFmpeg junto al ejecutable tras compilar.
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
add_custom_command(TARGET PepeVideoStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:PepeVideoStudio>"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

# En Windows, app GUI sin consola
set_target_properties(PepeVideoStudio PROPERTIES
    WIN32_EXECUTABLE ON
)
