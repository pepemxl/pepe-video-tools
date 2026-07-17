import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import PepeVideo

Window {
    id: win
    width: 1680
    height: 1012
    minimumWidth: 1100
    minimumHeight: 680
    visible: true
    color: Theme.bg0
    title: "PepeVideo Studio"
    flags: Qt.Window | Qt.FramelessWindowHint

    // ===== Atajos de teclado =====
    // Este contenedor es el ANCESTRO de toda la UI (no un hermano superpuesto),
    // así los eventos de tecla no consumidos por el elemento con foco (p. ej. un
    // clip seleccionado en la línea de tiempo) burbujean hacia arriba hasta aquí.
    // Un TextInput con foco consume antes Retroceso/Supr, así que editar textos
    // no dispara "eliminar clip"; solo se elimina cuando el foco NO es un campo.
    FocusScope {
        anchors.fill: parent
        focus: true
        Keys.onPressed: (e) => {
            // Undo / redo (Ctrl+Z, Ctrl+Shift+Z)
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_Z) {
                (e.modifiers & Qt.ShiftModifier) ? TimelineModel.redo() : TimelineModel.undo()
                e.accepted = true; return
            }
            if (e.key === Qt.Key_Space) {   // reproducir/pausar el monitor de PROGRAMA
                Compositor.togglePlay()
                e.accepted = true; return
            }
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_T) {  // añadir título
                TimelineModel.addTitleAtPlayhead()
                e.accepted = true; return
            }
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_E) {  // exportar vídeo
                if (!Export.running) Export.openExportDialog()
                e.accepted = true; return
            }
            // Proyecto: guardar / guardar como / abrir / nuevo
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_S) {
                (e.modifiers & Qt.ShiftModifier) ? Project.openSaveAsDialog() : Project.save()
                e.accepted = true; return
            }
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_O) {
                Project.openOpenDialog()
                e.accepted = true; return
            }
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_N) {
                Project.newProject()
                e.accepted = true; return
            }
            switch (e.key) {
            case Qt.Key_A: timeline.currentTool = 0; e.accepted = true; break;
            case Qt.Key_T: timeline.currentTool = 1; e.accepted = true; break;
            case Qt.Key_B: timeline.currentTool = 2; e.accepted = true; break;
            case Qt.Key_N: timeline.currentTool = 4; e.accepted = true; break;
            case Qt.Key_Y: timeline.currentTool = 5; e.accepted = true; break;
            case Qt.Key_U: timeline.currentTool = 6; e.accepted = true; break;
            case Qt.Key_W: timeline.currentTool = 7; e.accepted = true; break;
            case Qt.Key_P: timeline.currentTool = 8; e.accepted = true; break;
            case Qt.Key_Z: timeline.currentTool = 9; e.accepted = true; break;
            case Qt.Key_S: TimelineModel.snapEnabled = !TimelineModel.snapEnabled; e.accepted = true; break;
            case Qt.Key_M: TimelineModel.addMarkerAtPlayhead(); e.accepted = true; break;
            case Qt.Key_Delete:
            case Qt.Key_Backspace:
                (e.modifiers & Qt.ShiftModifier) ? TimelineModel.rippleDeleteSelected()
                                                 : TimelineModel.removeSelected()
                e.accepted = true; break;
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            TitleBar { Layout.fillWidth: true; win: win }
            TopBar   {
                id: topBar
                Layout.fillWidth: true
                // Workspaces: cada modo muestra los paneles relevantes (ver bindings
                // de visibilidad abajo). Fusión/Color fijan además la tab del Inspector.
                onCurrentModeChanged: {
                    if (currentMode === 2) inspector.currentTab = 0        // Fusión → Inspector
                    else if (currentMode === 3) inspector.currentTab = 2   // Color → corrección
                }
            }

            // Fila principal: media pool · monitores · inspector
            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 1
                MediaPool {
                    Layout.fillHeight: true
                    // Medios (0): panel protagonista, más ancho; Editar (1): ancho normal.
                    visible: topBar.currentMode <= 1
                    Layout.preferredWidth: topBar.currentMode === 0 ? 460 : 272
                }
                MonitorsRow  { Layout.fillWidth: true; Layout.fillHeight: true }
                Inspector {
                    id: inspector
                    Layout.fillHeight: true
                    // Editar / Fusión / Color; oculto en Medios, Audio y Entregar.
                    visible: topBar.currentMode >= 1 && topBar.currentMode <= 3
                }
            }

            // Sin timeline en Medios (0): esa vista es para revisar material.
            TimelinePanel { id: timeline; Layout.fillWidth: true; visible: topBar.currentMode !== 0 }
            StatusBar     { Layout.fillWidth: true }
        }
    }
}
