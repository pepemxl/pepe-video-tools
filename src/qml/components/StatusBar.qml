import QtQuick
import QtQuick.Layouts
import PepeVideo

// Barra de estado / exportación.
Rectangle {
    height: 38
    color: Theme.bg3

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        // Info del proyecto (nombre y secuencia reales del modelo de proyecto)
        Row {
            spacing: 16
            Text { text: Project.baseName + " · " + Project.seqWidth + "×" + Project.seqHeight
                   color: Theme.textMid; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: Project.seqFps.toFixed(2) + " fps"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: Project.seqColorSpace; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono }
            // Estado de guardado real: hora del último guardado (auto o manual) o aviso.
            Text {
                text: Project.lastSavedText !== ""
                          ? "● " + (Project.lastSaveAuto ? "Autoguardado " : "Guardado ") + Project.lastSavedText
                          : (Project.dirty ? "● Sin guardar" : "○ Sin cambios")
                color: Project.lastSavedText !== "" ? Theme.green
                                                    : (Project.dirty ? Theme.amber : Theme.textDim)
                font.pixelSize: 10; font.family: Theme.mono
            }
        }

        Item { Layout.fillWidth: true }

        // Estado de exportación (progreso o resultado del último render).
        Text {
            visible: Export.running || Export.status !== ""
            text: Export.running ? ("Exportando… " + Math.round(Export.progress * 100) + " %") : Export.status
            color: Export.running ? Theme.amber : Theme.textDim
            font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideMiddle; Layout.maximumWidth: 240
        }

        Text { text: "Exportar:"; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.sans }

        Repeater {
            model: [
                { t: "MP4 · H.264", c: Theme.text },
                { t: "1920×1080",  c: Theme.text },
                { t: "24 Mb/s",    c: Theme.text },
                { t: "Preset: YouTube 1080p", c: Theme.blue }
            ]
            delegate: Rectangle {
                required property var modelData
                height: 26; width: chipRow.width + 18; radius: 5
                color: Theme.sunken; border.color: Theme.line; border.width: 1
                Row {
                    id: chipRow; anchors.centerIn: parent; spacing: 5
                    Text { text: modelData.t; color: modelData.c; font.pixelSize: 10; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "▾"; color: Theme.textFaint; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                }
            }
        }

        // Botón exportar (abre el diálogo nativo de guardado y renderiza en 2.º plano).
        Rectangle {
            id: exportBtn
            height: 26; width: exp.width + 32; radius: 5; clip: true
            color: Export.running ? Theme.sunken : (exportHover.hovered ? Qt.lighter(Theme.amber, 1.08) : Theme.amber)
            border.color: Theme.amber; border.width: Export.running ? 1 : 0
            // Relleno de progreso durante la exportación.
            Rectangle { visible: Export.running; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                width: parent.width * Export.progress; color: "#553a2a0e" }
            HoverHandler { id: exportHover }
            TapHandler { enabled: !Export.running; onTapped: Export.openExportDialog() }
            Row {
                id: exp; anchors.centerIn: parent; spacing: 7
                Canvas {
                    width: 8; height: 10; anchors.verticalCenter: parent.verticalCenter
                    onPaint: { var c = getContext("2d"); c.fillStyle = Export.running ? Theme.amber : Theme.amberInk;
                        c.beginPath(); c.moveTo(0,0); c.lineTo(8,5); c.lineTo(0,10); c.closePath(); c.fill() }
                }
                Text { text: Export.running ? "Renderizando…" : "Exportar"
                       color: Export.running ? Theme.amber : Theme.amberInk
                       font.pixelSize: 11; font.weight: Font.Bold; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
            }
        }
    }
}
