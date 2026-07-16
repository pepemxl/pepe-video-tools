import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import PepeVideo

// Barra de menús (funcional) + tabs de modo (workspaces).
Rectangle {
    id: root
    height: 30
    color: Theme.panel2

    // Workspace activo (0 Medios · 1 Editar · 2 Fusión · 3 Color · 4 Audio · 5 Entregar).
    // Main.qml lo lee para mostrar/ocultar paneles.
    property alias currentMode: modeRow.current
    // Hook de prueba (PVS_WORKSPACE, ver main.cpp): workspace inicial.
    Component.onCompleted: if (typeof pvsInitialWorkspace !== "undefined") currentMode = pvsInitialWorkspace

    // Estado compartido: menú actualmente abierto (para cerrar/cambiar entre menús).
    QtObject { id: menuState; property var current: null }

    // ---- Ítem de menú estilizado (tema oscuro) ----
    component Mi: C.MenuItem {
        id: mi
        property string shortcut: ""
        property bool marked: false          // muestra ✓ (opciones conmutables)
        implicitHeight: 26
        implicitWidth: 240
        contentItem: RowLayout {
            spacing: 8
            Text { text: mi.marked ? "✓" : ""; color: Theme.amber; font.pixelSize: 12; Layout.preferredWidth: 10 }
            Text { text: mi.text; color: mi.enabled ? Theme.textHi : Theme.textFaint
                   font.pixelSize: 12; font.family: Theme.sans; Layout.fillWidth: true }
            Text { text: mi.shortcut; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono
                   visible: mi.shortcut !== "" }
        }
        background: Rectangle { color: mi.highlighted ? Theme.hover : "transparent"; radius: 4 }
    }
    // ---- Separador estilizado ----
    component Sep: C.MenuSeparator {
        padding: 5
        contentItem: Rectangle { implicitHeight: 1; color: Theme.line }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12

        // Menús
        Row {
            spacing: 2

            MenuButton { title: "Archivo"; state: menuState
                Mi { text: "Nuevo proyecto"; shortcut: "Ctrl+N"; onTriggered: Project.newProject() }
                Mi { text: "Abrir proyecto…"; shortcut: "Ctrl+O"; onTriggered: Project.openOpenDialog() }
                Mi { text: "Guardar"; shortcut: "Ctrl+S"; onTriggered: Project.save() }
                Mi { text: "Guardar como…"; shortcut: "Ctrl+Shift+S"; onTriggered: Project.openSaveAsDialog() }
                Sep {}
                Mi { text: "Importar medios…"; shortcut: "Ctrl+I"; onTriggered: MediaPoolModel.openImportDialog() }
                Sep {}
                Mi { text: "Importar subtítulos (.srt)…"; onTriggered: TimelineModel.openImportSrtDialog() }
                Mi { text: "Exportar subtítulos (.srt)…"; onTriggered: TimelineModel.openExportSrtDialog() }
                Sep {}
                Mi { text: "Exportar vídeo (MP4)…"; shortcut: "Ctrl+E"; enabled: !Export.running; onTriggered: Export.openExportDialog() }
                Sep {}
                Mi { text: "Salir"; shortcut: "Alt+F4"; onTriggered: Qt.quit() }
            }

            MenuButton { title: "Editar"; state: menuState
                Mi { text: "Deshacer"; shortcut: "Ctrl+Z"; enabled: TimelineModel.canUndo; onTriggered: TimelineModel.undo() }
                Mi { text: "Rehacer"; shortcut: "Ctrl+Shift+Z"; enabled: TimelineModel.canRedo; onTriggered: TimelineModel.redo() }
                Sep {}
                Mi { text: "Eliminar clip"; shortcut: "Supr"; enabled: TimelineModel.hasSelection; onTriggered: TimelineModel.removeSelected() }
            }

            MenuButton { title: "Clip"; state: menuState
                Mi { text: "Añadir título"; shortcut: "Ctrl+T"; onTriggered: TimelineModel.addTitleAtPlayhead() }
                Sep {}
                Mi { text: "Cortar en el playhead"; shortcut: "B"; enabled: TimelineModel.hasSelection; onTriggered: TimelineModel.splitSelectedAtPlayhead() }
                Mi { text: "Eliminar"; shortcut: "Supr"; enabled: TimelineModel.hasSelection; onTriggered: TimelineModel.removeSelected() }
            }

            MenuButton { title: "Secuencia"; state: menuState
                Mi { text: "Reproducir / Pausar"; shortcut: "Espacio"; onTriggered: Compositor.togglePlay() }
                Sep {}
                Mi { text: "Ir al inicio"; shortcut: "Inicio"; onTriggered: TimelineModel.goToStart() }
                Mi { text: "Ir al final"; shortcut: "Fin"; onTriggered: TimelineModel.goToEnd() }
            }

            MenuButton { title: "Marcar"; state: menuState
                Mi { text: "Añadir marcador"; shortcut: "M"; onTriggered: TimelineModel.addMarkerAtPlayhead() }
                Mi { text: "Quitar marcador cercano"; onTriggered: TimelineModel.removeMarkerNear(TimelineModel.playheadFraction) }
            }

            MenuButton { title: "Ver"; state: menuState
                Mi { text: "Imán (snap)"; shortcut: "S"; marked: TimelineModel.snapEnabled
                     onTriggered: TimelineModel.snapEnabled = !TimelineModel.snapEnabled }
                Mi { text: "Subtítulos (CC)"; marked: TimelineModel.subtitlesEnabled
                     onTriggered: TimelineModel.subtitlesEnabled = !TimelineModel.subtitlesEnabled }
            }

            MenuButton { title: "Ayuda"; state: menuState
                Mi { text: "PepeVideo Studio · v0.1.0"; enabled: false }
                Sep {}
                Mi { text: "Espacio · Reproducir / Pausar"; onTriggered: {} }
                Mi { text: "Ctrl+Z / Ctrl+Shift+Z · Deshacer / Rehacer"; onTriggered: {} }
                Mi { text: "Ctrl+T · Añadir título    ·    M · Marcador"; onTriggered: {} }
                Mi { text: "B · Cuchilla   S · Imán   Supr · Eliminar"; onTriggered: {} }
            }
        }

        Item { Layout.fillWidth: true }

        // Tabs de modo
        Rectangle {
            width: modeRow.width + 4; height: 24; radius: 6; color: Theme.sunken
            Row {
                id: modeRow
                anchors.centerIn: parent
                spacing: 1
                property int current: 1
                Repeater {
                    model: ["Medios", "Editar", "Fusión", "Color", "Audio", "Entregar"]
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        readonly property bool active: modeRow.current === index
                        width: t.width + 24; height: 20; radius: 4
                        color: active ? Theme.amber : "transparent"
                        HoverHandler { id: th }
                        TapHandler { onTapped: modeRow.current = index }
                        Text { id: t; anchors.centerIn: parent; text: modelData; font.pixelSize: 11
                               font.family: Theme.sans
                               font.weight: parent.active ? Font.DemiBold : Font.Normal
                               color: parent.active ? Theme.amberInk : (th.hovered ? Theme.text : "#8b8e97") }
                    }
                }
            }
        }
    }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
}
