cmake_minimum_required(VERSION 3.21)

project(PepeVideoStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick Gui)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(PepeVideoStudio
    src/main.cpp
    src/app/mediapoolmodel.h
    src/app/mediapoolmodel.cpp
)

qt_add_qml_module(PepeVideoStudio
    URI PepeVideo
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
        src/qml/components/TitleBar.qml
        src/qml/components/TopBar.qml
        src/qml/components/StatusBar.qml
        src/qml/panels/MediaPool.qml
        src/qml/panels/MonitorsRow.qml
        src/qml/panels/Inspector.qml
        src/qml/panels/TimelinePanel.qml
    QML_FILES
        src/qml/theme/Theme.qml
)

# Theme.qml es un singleton QML
set_source_files_properties(src/qml/theme/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

target_link_libraries(PepeVideoStudio PRIVATE Qt6::Quick Qt6::Gui)

# En Windows, app GUI sin consola
set_target_properties(PepeVideoStudio PROPERTIES
    WIN32_EXECUTABLE ON
)
