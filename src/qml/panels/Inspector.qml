import QtQuick
import QtQuick.Layouts
import PepeVideo

// Panel derecho: Inspector + Scopes + Color.
Rectangle {
    width: 322
    color: Theme.panel

    // Campo numérico editable por arrastre horizontal (relativo al valor al pulsar).
    component NumRow: RowLayout {
        id: nr
        property string label
        property string display
        property real value: 0
        property real sensitivity: 0.01
        signal edited(real v)
        Layout.fillWidth: true; spacing: 8
        Rectangle { width: 8; height: 8; rotation: 45; color: "transparent"; border.color: Theme.textFaint; border.width: 1.5 }
        Text { text: nr.label; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; Layout.preferredWidth: 64 }
        Rectangle {
            Layout.fillWidth: true; height: 24; radius: 4; color: Theme.sunken
            border.color: dragArea.pressed ? Theme.amber : Theme.line; border.width: 1
            Text { anchors.left: parent.left; anchors.leftMargin: 7; anchors.verticalCenter: parent.verticalCenter
                   text: nr.display; font.pixelSize: 11; font.family: Theme.mono; color: Theme.blue }
            MouseArea {
                id: dragArea; anchors.fill: parent; cursorShape: Qt.SizeHorCursor
                property real base
                property real px
                onPressed: (m) => { base = nr.value; px = m.x }
                onPositionChanged: (m) => nr.edited(base + (m.x - px) * nr.sensitivity)
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Tabs
        Rectangle {
            Layout.fillWidth: true; height: 34; color: Theme.panelHead
            Row {
                anchors.left: parent.left; anchors.leftMargin: 6; anchors.verticalCenter: parent.verticalCenter; spacing: 2
                property int current: 0
                Repeater {
                    model: ["Inspector", "Scopes", "Color"]
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        readonly property bool active: parent.current === index
                        width: it.width + 20; height: 24; radius: 4; color: active ? Theme.panel : "transparent"
                        Rectangle { visible: parent.active; width: parent.width; height: 2; color: Theme.amber; anchors.top: parent.top }
                        HoverHandler { id: ih }
                        TapHandler { onTapped: parent.parent.current = index }
                        Text { id: it; anchors.centerIn: parent; text: modelData; font.pixelSize: 11; font.family: Theme.sans
                               font.weight: parent.active ? Font.DemiBold : Font.Normal
                               color: parent.active ? Theme.textHi : (ih.hovered ? Theme.text : "#8b8e97") }
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        }

        // Cuerpo
        Flickable {
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: width; contentHeight: col.height; clip: true
            ColumnLayout {
                id: col; width: parent.width; spacing: 0

                // ---- Transformar ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 9
                    enabled: TimelineModel.hasSelection
                    opacity: TimelineModel.hasSelection ? 1.0 : 0.4
                    RowLayout { Layout.fillWidth: true
                        Text { text: "Transformar"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Text { text: TimelineModel.hasSelection ? TimelineModel.selectedName : "Sin selección"
                               color: Theme.textDim; font.pixelSize: 10; font.family: Theme.sans; elide: Text.ElideMiddle; Layout.maximumWidth: 150 } }

                    NumRow { label: "Posición X"; value: TimelineModel.selPosX; sensitivity: 0.002
                             display: (TimelineModel.selPosX * 1280).toFixed(0) + " px"
                             onEdited: (v) => TimelineModel.setSelPosX(v) }
                    NumRow { label: "Posición Y"; value: TimelineModel.selPosY; sensitivity: 0.002
                             display: (TimelineModel.selPosY * 720).toFixed(0) + " px"
                             onEdited: (v) => TimelineModel.setSelPosY(v) }
                    NumRow { label: "Escala"; value: TimelineModel.selScale; sensitivity: 0.01
                             display: (TimelineModel.selScale * 100).toFixed(1) + " %"
                             onEdited: (v) => TimelineModel.setSelScale(v) }
                    NumRow { label: "Rotación"; value: TimelineModel.selRotation; sensitivity: 0.5
                             display: TimelineModel.selRotation.toFixed(1) + "°"
                             onEdited: (v) => TimelineModel.setSelRotation(v) }

                    // Opacidad (deslizador)
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        Rectangle { width: 8; height: 8; rotation: 45; color: "transparent"; border.color: Theme.textFaint; border.width: 1.5 }
                        Text { text: "Opacidad"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; Layout.preferredWidth: 64 }
                        Rectangle { id: opTrack; Layout.fillWidth: true; height: 6; radius: 3; color: Theme.sunken
                            Rectangle { height: parent.height; radius: 3; color: "#4a4d55"; width: parent.width * TimelineModel.selOpacity }
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.text; x: parent.width * TimelineModel.selOpacity - 6; y: -3 }
                            MouseArea { anchors.fill: parent; anchors.margins: -5
                                onPressed: (m) => TimelineModel.setSelOpacity(m.x / opTrack.width)
                                onPositionChanged: (m) => TimelineModel.setSelOpacity(m.x / opTrack.width) } }
                        Text { text: (TimelineModel.selOpacity * 100).toFixed(0) + "%"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; Layout.preferredWidth: 30 }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Velocidad ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 8
                    Text { text: "Velocidad · Remapeo"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        Text { text: "Velocidad"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; Layout.preferredWidth: 64 }
                        Rectangle { Layout.fillWidth: true; height: 24; radius: 4; color: Theme.sunken; border.color: Theme.line; border.width: 1
                            Text { anchors.left: parent.left; anchors.leftMargin: 7; anchors.verticalCenter: parent.verticalCenter; text: "45.0 %"; font.pixelSize: 11; font.family: Theme.mono; color: Theme.text } }
                        Text { text: "◷ Fluido"; color: Theme.green; font.pixelSize: 10; font.family: Theme.sans } }
                    Rectangle { Layout.fillWidth: true; height: 30; radius: 4; color: Theme.sunken; border.color: Theme.line; border.width: 1; clip: true
                        Rectangle { width: parent.width*0.6; height: parent.height; color: "#443c6b4a" }
                        Rectangle { x: parent.width*0.6; width: parent.width*0.4; height: parent.height; color: "#22e2a24b" }
                        Rectangle { x: parent.width*0.6; width: 2; height: parent.height; color: Theme.amber } }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Corrección primaria ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 11
                    Text { text: "Corrección primaria"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: [
                                { n: "Sombras", num: "-0.04", dx: 0.44, dy: 0.38, amber: false },
                                { n: "Medios",  num: "+0.02", dx: 0.52, dy: 0.50, amber: false },
                                { n: "Altas",   num: "+0.08", dx: 0.58, dy: 0.44, amber: true }
                            ]
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true; spacing: 5
                                Rectangle {
                                    Layout.alignment: Qt.AlignHCenter
                                    width: 58; height: 58; radius: 29; border.color: Theme.line2; border.width: 1
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#3a3d45" }
                                        GradientStop { position: 1.0; color: "#141519" }
                                    }
                                    Rectangle { width: 6; height: 6; radius: 3; color: modelData.amber ? Theme.amber : Theme.text
                                                x: parent.width*modelData.dx - 3; y: parent.height*modelData.dy - 3 }
                                }
                                Text { Layout.alignment: Qt.AlignHCenter; text: modelData.n; color: Theme.textMid; font.pixelSize: 9; font.family: Theme.sans }
                                Text { Layout.alignment: Qt.AlignHCenter; text: modelData.num; color: Theme.textDim; font.pixelSize: 9; font.family: Theme.mono }
                            }
                        }
                    }
                    Repeater {
                        model: [ { l: "Temp", pos: 0.62, temp: true, sat: false },
                                 { l: "Tinte", pos: 0.50, temp: false, sat: false },
                                 { l: "Saturac.", pos: 0.64, temp: false, sat: true } ]
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 8
                            Text { text: modelData.l; color: Theme.textMid; font.pixelSize: 10; font.family: Theme.sans; Layout.preferredWidth: 56 }
                            Rectangle { Layout.fillWidth: true; height: 5; radius: 3; color: Theme.sunken
                                gradient: modelData.temp ? tempGrad : null
                                Gradient { id: tempGrad; orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: "#5b8dd6" } GradientStop { position: 0.5; color: "#4a4d55" } GradientStop { position: 1.0; color: "#e2a24b" } }
                                Rectangle { visible: modelData.sat; height: parent.height; radius: 3; color: Theme.green; width: parent.width*modelData.pos }
                                Rectangle { width: 11; height: 11; radius: 6; color: Theme.textHi; x: parent.width*modelData.pos - 5.5; y: -3 } }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Scopes ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 9
                    Text { text: "Scopes"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    Rectangle {
                        Layout.fillWidth: true; height: 72; radius: 4; color: Theme.sunken2; border.color: Theme.line; border.width: 1; clip: true
                        Text { text: "WAVEFORM"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono; x: 6; y: 4 }
                        Rectangle { anchors.centerIn: parent; width: parent.width*0.86; height: 34; color: "#184a9e6b"
                            Rectangle { anchors.centerIn: parent; width: parent.width*0.7; height: 18; color: "#454a9e6b"
                                Rectangle { anchors.centerIn: parent; width: parent.width*0.45; height: 14; color: "#7fd6a0" } } }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        Rectangle {
                            Layout.fillWidth: true; height: 78; radius: 4; color: Theme.sunken2; border.color: Theme.line; border.width: 1
                            Text { text: "VECTOR"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono; x: 6; y: 4 }
                            Rectangle { anchors.centerIn: parent; width: 56; height: 56; radius: 28; color: "transparent"; border.color: Theme.line; border.width: 1
                                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; height: parent.height; width: 1; color: "#10ffffff" }
                                Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 1; color: "#10ffffff" }
                                Rectangle { width: 16; height: 12; radius: 6; color: "#e2a24b"; opacity: 0.7; x: parent.width*0.52; y: parent.height*0.40 } }
                        }
                        Rectangle {
                            Layout.fillWidth: true; height: 78; radius: 4; color: Theme.sunken2; border.color: Theme.line; border.width: 1; clip: true
                            Text { text: "HISTOGRAM"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono; x: 6; y: 4 }
                            Canvas { anchors.fill: parent; onPaint: {
                                var c = getContext("2d"); c.clearRect(0,0,width,height)
                                var pts = [[0,78],[8,60],[18,50],[30,30],[42,22],[55,26],[68,40],[80,55],[92,68],[100,78]]
                                var sx = width/100, sy = height/78
                                c.beginPath(); c.moveTo(pts[0][0]*sx,pts[0][1]*sy)
                                for (var i=1;i<pts.length;i++) c.lineTo(pts[i][0]*sx,pts[i][1]*sy)
                                c.closePath(); c.fillStyle = "#5b8dd640"; c.fill(); c.strokeStyle = Theme.blue; c.lineWidth = 1; c.stroke() } }
                        }
                    }
                }
            }
        }
    }
}
