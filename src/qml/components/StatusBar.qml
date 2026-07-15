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

        // Info del proyecto
        Row {
            spacing: 16
            Text { text: "Vlog 04 · 1920×1080"; color: Theme.textMid; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: "29.97 fps"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: "Rec.709"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: "● Autoguardado 14:32"; color: Theme.green; font.pixelSize: 10; font.family: Theme.mono }
        }

        Item { Layout.fillWidth: true }

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

        // Botón exportar
        Rectangle {
            height: 26; width: exp.width + 32; radius: 5; color: Theme.amber
            Row {
                id: exp; anchors.centerIn: parent; spacing: 7
                Canvas {
                    width: 8; height: 10; anchors.verticalCenter: parent.verticalCenter
                    onPaint: { var c = getContext("2d"); c.fillStyle = Theme.amberInk;
                        c.beginPath(); c.moveTo(0,0); c.lineTo(8,5); c.lineTo(0,10); c.closePath(); c.fill() }
                }
                Text { text: "Exportar"; color: Theme.amberInk; font.pixelSize: 11; font.weight: Font.Bold; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
            }
        }
    }
}
