import QtQuick
import QtQuick.Layouts
import PepeVideo

// Panel izquierdo: Media Pool (Fase 1: medios reales vía MediaPoolModel).
Rectangle {
    width: 272
    color: Theme.panel

    // Botón de preset reutilizable (Efectos/Títulos).
    component PresetBtn: Rectangle {
        id: pb
        property string label
        property string sub: ""
        property bool enabledBtn: true
        signal clicked()
        Layout.fillWidth: true; height: 42; radius: 5
        opacity: enabledBtn ? 1.0 : 0.4
        color: pbHover.hovered && enabledBtn ? Theme.hover : Theme.sunken
        border.color: Theme.line; border.width: 1
        HoverHandler { id: pbHover }
        TapHandler { enabled: pb.enabledBtn; onTapped: pb.clicked() }
        Column { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; spacing: 2
            Text { text: pb.label; color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans; font.weight: Font.DemiBold }
            Text { visible: pb.sub !== ""; text: pb.sub; color: Theme.textDim; font.pixelSize: 9; font.family: Theme.sans } }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Tabs
        Rectangle {
            Layout.fillWidth: true; height: 34; color: Theme.panelHead
            Row {
                id: poolTabs
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
            visible: poolTabs.current === 0
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
            visible: poolTabs.current === 0
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

        Rectangle { visible: poolTabs.current === 0; Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.topMargin: 2; height: 1; color: Theme.line }

        // Grid de miniaturas (modelo C++)
        GridView {
            id: grid
            visible: poolTabs.current === 0
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
                required property string path
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

                    // Fantasma de arrastre: transporta los datos del medio a la línea de tiempo.
                    Item {
                        id: dragGhost
                        width: 120; height: 68
                        Drag.active: tileMA.drag.active
                        Drag.hotSpot.x: width / 2; Drag.hotSpot.y: height / 2
                        Drag.keys: ["application/x-pvs-media"]
                        Drag.mimeData: ({
                            "application/x-pvs-media": cell.path,
                            "text/pvs-name": cell.nm,
                            "text/pvs-kind": cell.kind,
                            "text/pvs-dur": cell.dur
                        })
                        Rectangle {
                            anchors.fill: parent; radius: 4; visible: dragGhost.Drag.active
                            color: "#e61c1d22"; border.color: Theme.amber; border.width: 1
                            Text { anchors.centerIn: parent; width: parent.width - 8; text: cell.nm
                                   color: Theme.textHi; font.pixelSize: 9; font.family: Theme.sans
                                   elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                        }
                    }
                    MouseArea {
                        id: tileMA
                        anchors.fill: parent
                        drag.target: dragGhost
                        onPressed: { dragGhost.x = 0; dragGhost.y = 0; MediaPoolModel.selectedIndex = cell.index }
                        onReleased: dragGhost.Drag.drop()
                        onDoubleClicked: if (cell.path !== "") VideoController.open(cell.path)
                    }
                }
                Text { width: parent.width; text: cell.nm; elide: Text.ElideRight; font.pixelSize: 10; font.family: Theme.sans
                       color: (cell.selected || cell.used) ? Theme.textHi : Theme.textMid }
            }
        }

        // Metadatos del clip seleccionado
        Rectangle {
            visible: poolTabs.current === 0
            Layout.fillWidth: true; height: 52; color: Theme.panel3
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
            Column {
                anchors.fill: parent; anchors.leftMargin: 10; anchors.topMargin: 7; spacing: 2
                Text { text: MediaPoolModel.selectedName; color: "#c6c9d0"; font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width - 12 }
                Text { text: MediaPoolModel.selectedLine1; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width - 12 }
                Text { text: MediaPoolModel.selectedLine2; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width - 12 }
            }
        }

        // ---- Pestaña Efectos: presets de color aplicados al clip seleccionado ----
        ColumnLayout {
            visible: poolTabs.current === 1
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.margins: 8; spacing: 6
            Text { text: TimelineModel.hasSelection ? "Aplicar al clip seleccionado" : "Selecciona un clip primero"
                   color: TimelineModel.hasSelection ? Theme.textMid : Theme.textFaint
                   font.pixelSize: 10; font.family: Theme.sans; Layout.bottomMargin: 2 }
            PresetBtn { label: "Restablecer color"; sub: "Neutro"; enabledBtn: TimelineModel.hasSelection
                        onClicked: TimelineModel.resetSelColor() }
            PresetBtn { label: "Blanco y negro"; sub: "Saturación 0"; enabledBtn: TimelineModel.hasSelection
                        onClicked: { TimelineModel.resetSelColor(); TimelineModel.setSelSat(0) } }
            PresetBtn { label: "Cálido"; sub: "Temp +"; enabledBtn: TimelineModel.hasSelection
                        onClicked: { TimelineModel.resetSelColor(); TimelineModel.setSelTemp(0.4) } }
            PresetBtn { label: "Frío"; sub: "Temp −"; enabledBtn: TimelineModel.hasSelection
                        onClicked: { TimelineModel.resetSelColor(); TimelineModel.setSelTemp(-0.4) } }
            PresetBtn { label: "Vívido"; sub: "Saturación +"; enabledBtn: TimelineModel.hasSelection
                        onClicked: { TimelineModel.resetSelColor(); TimelineModel.setSelSat(1.4) } }
            PresetBtn { label: "Cine"; sub: "Cálido · desaturado"; enabledBtn: TimelineModel.hasSelection
                        onClicked: { TimelineModel.resetSelColor(); TimelineModel.setSelTemp(0.15); TimelineModel.setSelTint(-0.08); TimelineModel.setSelSat(0.88) } }
            Item { Layout.fillHeight: true }
        }

        // ---- Pestaña Títulos: presets que insertan un título en el playhead ----
        ColumnLayout {
            visible: poolTabs.current === 2
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.margins: 8; spacing: 6
            Text { text: "Insertar en el playhead (pista V3)"; color: Theme.textMid
                   font.pixelSize: 10; font.family: Theme.sans; Layout.bottomMargin: 2 }
            PresetBtn { label: "Título centrado"; sub: "Texto grande centrado"
                        onClicked: { TimelineModel.addTitleAtPlayhead(); TimelineModel.setSelTitleAlign(1)
                                     TimelineModel.setSelTitleSize(0.11); TimelineModel.setSelPosY(0) } }
            PresetBtn { label: "Lower third"; sub: "Barra inferior izquierda"
                        onClicked: { TimelineModel.addTitleAtPlayhead(); TimelineModel.setSelTitleBar(true)
                                     TimelineModel.setSelTitleAlign(0); TimelineModel.setSelTitleSize(0.06); TimelineModel.setSelPosY(0.3) } }
            PresetBtn { label: "Subtítulo"; sub: "Texto pequeño inferior"
                        onClicked: { TimelineModel.addTitleAtPlayhead(); TimelineModel.setSelTitleAlign(1)
                                     TimelineModel.setSelTitleSize(0.05); TimelineModel.setSelPosY(0.4) } }
            Item { Layout.fillHeight: true }
        }
    }
}
