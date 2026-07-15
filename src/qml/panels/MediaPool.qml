import QtQuick
import QtQuick.Layouts
import PepeVideo

// Panel izquierdo: Media Pool (Fase 1: medios reales vía MediaPoolModel).
Rectangle {
    width: 272
    color: Theme.panel

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Tabs
        Rectangle {
            Layout.fillWidth: true; height: 34; color: Theme.panelHead
            Row {
                anchors.left: parent.left; anchors.leftMargin: 6; anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                property int current: 0
                Repeater {
                    model: ["Medios", "Efectos", "Títulos"]
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        readonly property bool active: parent.current === index
                        width: pt.width + 20; height: 24
                        radius: 4; color: active ? Theme.panel : "transparent"
                        Rectangle { visible: parent.active; width: parent.width; height: 2; color: Theme.amber; anchors.top: parent.top }
                        HoverHandler { id: ph }
                        TapHandler { onTapped: parent.parent.current = index }
                        Text { id: pt; anchors.centerIn: parent; text: modelData; font.pixelSize: 11; font.family: Theme.sans
                               font.weight: parent.active ? Font.DemiBold : Font.Normal
                               color: parent.active ? Theme.textHi : (ph.hovered ? Theme.text : "#8b8e97") }
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        }

        // Búsqueda + importar
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 8; spacing: 6
            Rectangle {
                Layout.fillWidth: true; height: 28; radius: 5
                color: Theme.sunken; border.color: Theme.line; border.width: 1
                Row {
                    anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; spacing: 6
                    Rectangle { width: 11; height: 11; radius: 6; color: "transparent"; border.color: Theme.textFaint; border.width: 1.5; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "Buscar clips…"; color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                }
            }
            Rectangle {
                width: 28; height: 28; radius: 5; color: addHover.hovered ? Qt.lighter(Theme.amber, 1.1) : Theme.amber
                HoverHandler { id: addHover }
                TapHandler { onTapped: MediaPoolModel.openImportDialog() }
                Text { anchors.centerIn: parent; text: "+"; color: Theme.amberInk; font.pixelSize: 18; font.weight: Font.DemiBold }
            }
        }

        // Bins
        ColumnLayout {
            Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; spacing: 1
            Rectangle {
                Layout.fillWidth: true; height: 24; radius: 4; color: "#2a2c33"
                Row {
                    anchors.left: parent.left; anchors.leftMargin: 6; anchors.verticalCenter: parent.verticalCenter; spacing: 7
                    Text { text: "▾"; color: Theme.amber; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: "Vlog 04 · Oaxaca"; color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                }
                Text { text: MediaPoolModel.count + " clips"; color: Theme.textFaint; font.pixelSize: 10; anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter }
            }
            Repeater {
                model: [
                    { n: "Cámara A · Diálogo", c: "#5b8dd6" },
                    { n: "B-roll · Mercado",   c: "#4a9e6b" },
                    { n: "Drone",              c: "#c98a3e" },
                    { n: "Música",             c: "#8a6bc0" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true; height: 22; radius: 4
                    color: bh.hovered ? Theme.hover2 : "transparent"
                    HoverHandler { id: bh }
                    Row {
                        anchors.left: parent.left; anchors.leftMargin: 22; anchors.verticalCenter: parent.verticalCenter; spacing: 7
                        Rectangle { width: 8; height: 8; radius: 2; color: modelData.c; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: modelData.n; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.topMargin: 2; height: 1; color: Theme.line }

        // Grid de miniaturas (modelo C++)
        GridView {
            id: grid
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 8
            clip: true
            cellWidth: (272 - 16) / 2; cellHeight: cellWidth * 9 / 16 + 20
            model: MediaPoolModel
            delegate: Column {
                id: cell
                required property int index
                required property string nm
                required property string dur
                required property string tex
                required property string kind
                required property string thumb
                required property bool used
                readonly property bool selected: MediaPoolModel.selectedIndex === index
                width: grid.cellWidth - 8
                spacing: 3
                Rectangle {
                    width: parent.width; height: width * 9 / 16; radius: 4
                    color: cell.tex
                    border.color: cell.selected ? Theme.amber : (cell.used ? Theme.amber : Theme.line2)
                    border.width: (cell.selected || cell.used) ? 2 : 1
                    clip: true
                    Image {
                        anchors.fill: parent; source: cell.thumb; visible: cell.thumb !== ""
                        fillMode: Image.PreserveAspectCrop; asynchronous: true; cache: true
                    }
                    Text { visible: cell.kind === "audio" && cell.thumb === ""; anchors.centerIn: parent
                           text: "♪ WAV"; color: Theme.purple; font.pixelSize: 9; font.family: Theme.mono }
                    Rectangle { visible: cell.used; anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 2
                        width: bt.width + 6; height: 12; radius: 2; color: Theme.amber
                        Text { id: bt; anchors.centerIn: parent; text: "EN USO"; color: Theme.amberInk; font.pixelSize: 8; font.weight: Font.DemiBold } }
                    Text { visible: cell.dur !== ""; text: cell.dur; color: "#c6c9d0"; font.pixelSize: 8; font.family: Theme.mono
                           anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 2 }
                    TapHandler { onTapped: MediaPoolModel.selectedIndex = cell.index }
                }
                Text { width: parent.width; text: cell.nm; elide: Text.ElideRight; font.pixelSize: 10; font.family: Theme.sans
                       color: (cell.selected || cell.used) ? Theme.textHi : Theme.textMid }
            }
        }

        // Metadatos del clip seleccionado
        Rectangle {
            Layout.fillWidth: true; height: 52; color: Theme.panel3
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
            Column {
                anchors.fill: parent; anchors.leftMargin: 10; anchors.topMargin: 7; spacing: 2
                Text { text: MediaPoolModel.selectedName; color: "#c6c9d0"; font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width - 12 }
                Text { text: MediaPoolModel.selectedLine1; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width - 12 }
                Text { text: MediaPoolModel.selectedLine2; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width - 12 }
            }
        }
    }
}
