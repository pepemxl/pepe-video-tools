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
            // Búsqueda real: filtra el Media Pool por nombre (MediaPoolModel.filter).
            Rectangle {
                Layout.fillWidth: true; height: 28; radius: 5
                color: Theme.sunken
                border.color: searchInput.activeFocus ? Theme.amber : Theme.line; border.width: 1
                Rectangle { width: 11; height: 11; radius: 6; color: "transparent"
                    border.color: Theme.textFaint; border.width: 1.5
                    anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter }
                TextInput {
                    id: searchInput
                    anchors.fill: parent; anchors.leftMargin: 25; anchors.rightMargin: 22
                    verticalAlignment: Text.AlignVCenter; clip: true; selectByMouse: true
                    color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans
                    onTextChanged: MediaPoolModel.filter = text
                    // Esc limpia la búsqueda y suelta el foco (los atajos vuelven a la app).
                    Keys.onEscapePressed: { text = ""; focus = false }
                }
                Text { visible: searchInput.text === "" && !searchInput.activeFocus
                       text: "Buscar clips…"; color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.sans
                       anchors.left: parent.left; anchors.leftMargin: 25; anchors.verticalCenter: parent.verticalCenter }
                // Botón ✕ para limpiar
                Rectangle {
                    visible: searchInput.text !== ""
                    width: 14; height: 14; radius: 7; color: clearHover.hovered ? Theme.hover : "transparent"
                    anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter
                    HoverHandler { id: clearHover }
                    TapHandler { onTapped: searchInput.text = "" }
                    Text { anchors.centerIn: parent; text: "✕"; color: Theme.textDim; font.pixelSize: 9 }
                }
            }
            Rectangle {
                width: 28; height: 28; radius: 5; color: addHover.hovered ? Qt.lighter(Theme.amber, 1.1) : Theme.amber
                HoverHandler { id: addHover }
                TapHandler { onTapped: MediaPoolModel.openImportDialog() }
                Text { anchors.centerIn: parent; text: "+"; color: Theme.amberInk; font.pixelSize: 18; font.weight: Font.DemiBold }
            }
        }

        // Bins reales: clic filtra la rejilla; arrastrar un tile a un bin lo asigna.
        ColumnLayout {
            visible: poolTabs.current === 0
            Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; spacing: 1
            // Raíz "Todos los medios" (proyecto) + botón de nuevo bin
            Rectangle {
                Layout.fillWidth: true; height: 24; radius: 4
                color: MediaPoolModel.currentBin === -1 ? "#2a2c33" : (allHover.hovered ? Theme.hover2 : "transparent")
                HoverHandler { id: allHover }
                TapHandler { onTapped: MediaPoolModel.currentBin = -1 }
                Row {
                    anchors.left: parent.left; anchors.leftMargin: 6; anchors.verticalCenter: parent.verticalCenter; spacing: 7
                    Text { text: "▾"; color: Theme.amber; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: Project.baseName; color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                }
                Row {
                    anchors.right: parent.right; anchors.rightMargin: 4; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                    Text { text: MediaPoolModel.count + " clips"; color: Theme.textFaint; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                    // Nuevo bin
                    Rectangle {
                        width: 16; height: 16; radius: 3
                        color: addBinHover.hovered ? Theme.hover : "transparent"
                        anchors.verticalCenter: parent.verticalCenter
                        HoverHandler { id: addBinHover }
                        TapHandler { onTapped: MediaPoolModel.addBin("") }
                        Text { anchors.centerIn: parent; text: "+"; color: Theme.textMid; font.pixelSize: 12 }
                    }
                }
            }
            Repeater {
                model: MediaPoolModel.bins
                delegate: Rectangle {
                    id: binRow
                    required property var modelData
                    readonly property bool active: MediaPoolModel.currentBin === modelData.index
                    Layout.fillWidth: true; height: 22; radius: 4
                    color: active ? "#2f3138" : (bh.hovered || binDrop.containsDrag ? Theme.hover2 : "transparent")
                    border.color: binDrop.containsDrag ? Theme.amber : "transparent"; border.width: 1
                    property bool editing: false
                    function startEdit() {
                        editing = true
                        binEdit.text = modelData.name
                        binEdit.forceActiveFocus()
                        binEdit.selectAll()
                    }
                    function commitEdit() {
                        if (!editing)
                            return
                        editing = false
                        MediaPoolModel.renameBin(modelData.index, binEdit.text)
                    }
                    HoverHandler { id: bh }
                    // Clic: filtra por este bin (clic de nuevo = volver a todos).
                    // Doble clic: renombrar en línea.
                    TapHandler {
                        enabled: !binRow.editing
                        exclusiveSignals: TapHandler.SingleTap | TapHandler.DoubleTap
                        onSingleTapped: MediaPoolModel.currentBin = binRow.active ? -1 : binRow.modelData.index
                        onDoubleTapped: binRow.startEdit()
                    }
                    // Soltar un medio aquí lo asigna al bin.
                    DropArea {
                        id: binDrop
                        anchors.fill: parent
                        keys: ["application/x-pvs-media"]
                        onDropped: (drop) => {
                            if (drop.source && drop.source.mediaId !== undefined)
                                MediaPoolModel.moveToBin(drop.source.mediaId, binRow.modelData.index)
                        }
                    }
                    Row {
                        id: binLabel
                        // Sangría por profundidad (bins anidados).
                        anchors.left: parent.left; anchors.leftMargin: 22 + binRow.modelData.depth * 12
                        anchors.verticalCenter: parent.verticalCenter; spacing: 7
                        Rectangle { width: 8; height: 8; radius: 2; color: binRow.modelData.color; anchors.verticalCenter: parent.verticalCenter }
                        Text { visible: !binRow.editing
                               text: binRow.modelData.name
                               color: binRow.active ? Theme.textHi : Theme.textMid
                               font.pixelSize: 11; font.family: Theme.sans
                               font.weight: binRow.active ? Font.DemiBold : Font.Normal
                               anchors.verticalCenter: parent.verticalCenter }
                    }
                    // Editor de renombrado (doble clic): Enter confirma, Esc cancela.
                    TextInput {
                        id: binEdit
                        visible: binRow.editing
                        anchors.left: binLabel.left; anchors.leftMargin: 15
                        anchors.right: parent.right; anchors.rightMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        clip: true; selectByMouse: true
                        color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans
                        selectionColor: Theme.amber; selectedTextColor: Theme.amberInk
                        onAccepted: binRow.commitEdit()
                        Keys.onEscapePressed: { binRow.editing = false; focus = false }
                        onActiveFocusChanged: if (!activeFocus) binRow.commitEdit()
                    }
                    // Contador (o + sub-bin / ✕ eliminar, al pasar el ratón)
                    Text {
                        visible: !bh.hovered && !binRow.editing
                        text: binRow.modelData.count
                        color: Theme.textFaint; font.pixelSize: 10; font.family: Theme.mono
                        anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                    }
                    Row {
                        visible: bh.hovered && !binRow.editing
                        spacing: 2
                        anchors.right: parent.right; anchors.rightMargin: 5; anchors.verticalCenter: parent.verticalCenter
                        // Nuevo sub-bin (anidado bajo este bin)
                        Rectangle {
                            width: 14; height: 14; radius: 3
                            color: subBinHover.hovered ? Theme.hover : "transparent"
                            anchors.verticalCenter: parent.verticalCenter
                            HoverHandler { id: subBinHover }
                            TapHandler { onTapped: MediaPoolModel.addBin("", binRow.modelData.index) }
                            Text { anchors.centerIn: parent; text: "+"; color: subBinHover.hovered ? Theme.textHi : Theme.textDim; font.pixelSize: 11 }
                        }
                        Rectangle {
                            width: 14; height: 14; radius: 3
                            color: delBinHover.hovered ? Theme.red : "transparent"
                            anchors.verticalCenter: parent.verticalCenter
                            HoverHandler { id: delBinHover }
                            TapHandler { onTapped: MediaPoolModel.removeBin(binRow.modelData.index) }
                            Text { anchors.centerIn: parent; text: "✕"; color: delBinHover.hovered ? "#ffffff" : Theme.textDim; font.pixelSize: 9 }
                        }
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
            // Sin resultados con el filtro activo
            Text {
                visible: MediaPoolModel.count === 0 && MediaPoolModel.filter !== ""
                anchors.centerIn: parent
                text: "Sin resultados para «" + MediaPoolModel.filter + "»"
                color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.sans
            }
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
                required property var mid
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

                    // Fantasma de arrastre: transporta los datos del medio a la línea de
                    // tiempo y a los bins. OJO: los arrastres internos de Qt Quick NO
                    // entregan Drag.mimeData al DropArea (solo drags "Automatic"); el
                    // payload viaja como PROPIEDADES de este item (drop.source).
                    Item {
                        id: dragGhost
                        width: 120; height: 68
                        readonly property string mediaPath: cell.path
                        readonly property string mediaName: cell.nm
                        readonly property string mediaKind: cell.kind
                        readonly property string mediaDur: cell.dur
                        readonly property var mediaId: cell.mid
                        Drag.active: tileMA.drag.active
                        Drag.hotSpot.x: width / 2; Drag.hotSpot.y: height / 2
                        Drag.keys: ["application/x-pvs-media"]
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
                        // Evita que el GridView (Flickable) robe el grab en arrastres
                        // largos: sin esto el drag se cancela y el drop nunca llega.
                        preventStealing: true
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
