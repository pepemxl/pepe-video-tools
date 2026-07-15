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

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar { Layout.fillWidth: true; win: win }
        TopBar   { Layout.fillWidth: true }

        // Fila principal: media pool · monitores · inspector
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 1
            MediaPool    { Layout.fillHeight: true }
            MonitorsRow  { Layout.fillWidth: true; Layout.fillHeight: true }
            Inspector    { Layout.fillHeight: true }
        }

        TimelinePanel { id: timeline; Layout.fillWidth: true }
        StatusBar     { Layout.fillWidth: true }
    }

    // ===== Atajos de teclado =====
    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: (e) => {
            // Undo / redo (Ctrl+Z, Ctrl+Shift+Z)
            if (e.modifiers & Qt.ControlModifier && e.key === Qt.Key_Z) {
                (e.modifiers & Qt.ShiftModifier) ? TimelineModel.redo() : TimelineModel.undo()
                e.accepted = true; return
            }
            switch (e.key) {
            case Qt.Key_A: timeline.currentTool = 0; break;
            case Qt.Key_T: timeline.currentTool = 1; break;
            case Qt.Key_B: timeline.currentTool = 2; break;
            case Qt.Key_N: timeline.currentTool = 4; break;
            case Qt.Key_Y: timeline.currentTool = 5; break;
            case Qt.Key_W: timeline.currentTool = 6; break;
            case Qt.Key_P: timeline.currentTool = 7; break;
            case Qt.Key_Z: timeline.currentTool = 8; break;
            case Qt.Key_S: TimelineModel.snapEnabled = !TimelineModel.snapEnabled; break;
            case Qt.Key_M: TimelineModel.addMarkerAtPlayhead(); break;
            case Qt.Key_Delete:
            case Qt.Key_Backspace: TimelineModel.removeSelected(); break;
            }
        }
    }
}
