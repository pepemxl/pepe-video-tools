cmake_minimum_required(VERSION 3.21)

project(PepeVideoStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick Gui Multimedia)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(PepeVideoStudio
    src/main.cpp
    src/app/theme.h
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
    src/engine/videosurface.h
    src/engine/videosurface.cpp
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
        src/qml/panels/MediaPool.qml
        src/qml/panels/MonitorsRow.qml
        src/qml/panels/Inspector.qml
        src/qml/panels/TimelinePanel.qml
)

# `Theme` es un singleton de C++ (src/app/theme.h) registrado en main.cpp.

target_link_libraries(PepeVideoStudio PRIVATE Qt6::Quick Qt6::Gui Qt6::Multimedia)

# Diálogo nativo de archivos en Windows (GetOpenFileNameW)
if (WIN32)
    target_link_libraries(PepeVideoStudio PRIVATE comdlg32)
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
