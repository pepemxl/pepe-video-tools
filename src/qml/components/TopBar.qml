import QtQuick
import QtQuick.Layouts
import PepeVideo

// Barra de menús + tabs de modo.
Rectangle {
    height: 30
    color: Theme.panel2

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12

        // Menús
        Row {
            spacing: 2
            Repeater {
                model: ["Archivo", "Editar", "Clip", "Secuencia", "Marcar", "Ver", "Ayuda"]
                delegate: Rectangle {
                    required property string modelData
                    width: lbl.width + 18; height: 24; radius: 4
                    color: mh.hovered ? Theme.hover : "transparent"
                    HoverHandler { id: mh }
                    Text { id: lbl; anchors.centerIn: parent; text: modelData; font.pixelSize: 12
                           color: mh.hovered ? "#ffffff" : "#b3b6be"; font.family: Theme.sans }
                }
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
