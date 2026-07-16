import QtQuick
import QtQuick.Layouts
import PepeVideo

// Panel derecho: Inspector + Scopes + Color.
Rectangle {
    width: 322
    color: Theme.panel

    // Campo numérico editable por arrastre horizontal (relativo al valor al pulsar).
    // El rombo es un toggle de keyframe en el playhead (relleno si hay uno aquí).
    component NumRow: RowLayout {
        id: nr
        property string label
        property string display
        property real value: 0
        property real sensitivity: 0.01
        property string prop            // propiedad para keyframes ("posX", ...)
        signal edited(real v)
        // hasSelection/playheadUs fuerzan la reevaluación (selectionChanged / playheadChanged).
        readonly property bool animated: { TimelineModel.hasSelection; return nr.prop !== "" && TimelineModel.isKeyframed(nr.prop) }
        readonly property bool kfHere: { TimelineModel.hasSelection; TimelineModel.playheadUs; return nr.prop !== "" && TimelineModel.hasKeyframeAtPlayhead(nr.prop) }
        Layout.fillWidth: true; spacing: 8
        Rectangle {
            width: 8; height: 8; rotation: 45
            color: nr.animated && nr.kfHere ? Theme.amber : "transparent"
            border.color: nr.animated ? Theme.amber : Theme.textFaint; border.width: 1.5
            TapHandler { enabled: nr.prop !== ""; onTapped: TimelineModel.toggleKeyframe(nr.prop) }
        }
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

    // Rueda de color: punto arrastrable dentro de un círculo → (x,y) en [-1,1].
    // Doble clic = centrar (neutro).
    component ColorWheel: ColumnLayout {
        id: cw
        property string label
        property real vx: 0
        property real vy: 0
        signal moved(real x, real y)
        Layout.fillWidth: true; spacing: 5
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 58; height: 58; radius: 29; border.color: Theme.line2; border.width: 1
            gradient: Gradient { GradientStop { position: 0.0; color: "#3a3d45" } GradientStop { position: 1.0; color: "#141519" } }
            Rectangle { anchors.centerIn: parent; width: 1; height: parent.height - 8; color: "#18ffffff" }
            Rectangle { anchors.centerIn: parent; width: parent.width - 8; height: 1; color: "#18ffffff" }
            Rectangle {
                width: 8; height: 8; radius: 4; color: Theme.amber; border.color: "#000000"; border.width: 1
                x: parent.width / 2 + cw.vx * (parent.width / 2 - 5) - 4
                y: parent.height / 2 - cw.vy * (parent.height / 2 - 5) - 4
            }
            MouseArea {
                anchors.fill: parent
                function upd(mx, my) {
                    var rad = width / 2 - 5
                    var nx = (mx - width / 2) / rad, ny = -(my - height / 2) / rad
                    var r = Math.sqrt(nx * nx + ny * ny)
                    if (r > 1) { nx /= r; ny /= r }
                    cw.moved(nx, ny)
                }
                onPressed: (m) => upd(m.x, m.y)
                onPositionChanged: (m) => upd(m.x, m.y)
                onDoubleClicked: cw.moved(0, 0)
            }
        }
        Text { Layout.alignment: Qt.AlignHCenter; text: cw.label; color: Theme.textMid; font.pixelSize: 9; font.family: Theme.sans }
    }

    // Deslizador simple (temp/tint/sat). Doble clic = valor por defecto.
    component CSlider: RowLayout {
        id: sl
        property string label
        property real value: 0
        property real from: -1
        property real to: 1
        property real defaultVal: 0
        property string prop            // propiedad para keyframes (vacío = sin automatización)
        signal moved(real v)
        readonly property bool animated: { TimelineModel.hasSelection; return sl.prop !== "" && TimelineModel.isKeyframed(sl.prop) }
        readonly property bool kfHere: { TimelineModel.hasSelection; TimelineModel.playheadUs; return sl.prop !== "" && TimelineModel.hasKeyframeAtPlayhead(sl.prop) }
        Layout.fillWidth: true; spacing: 8
        Rectangle {
            visible: sl.prop !== ""; width: 8; height: 8; rotation: 45
            color: sl.animated && sl.kfHere ? Theme.amber : "transparent"
            border.color: sl.animated ? Theme.amber : Theme.textFaint; border.width: 1.5
            TapHandler { onTapped: TimelineModel.toggleKeyframe(sl.prop) }
        }
        Text { text: sl.label; color: Theme.textMid; font.pixelSize: 10; font.family: Theme.sans; Layout.preferredWidth: 56 }
        Rectangle {
            id: tr; Layout.fillWidth: true; height: 5; radius: 3; color: Theme.sunken
            readonly property real frac: (sl.value - sl.from) / (sl.to - sl.from)
            Rectangle { width: 11; height: 11; radius: 6; color: Theme.textHi; x: tr.width * tr.frac - 5.5; y: -3 }
            MouseArea {
                anchors.fill: parent; anchors.margins: -5
                function upd(mx) { var f = Math.max(0, Math.min(1, mx / tr.width)); sl.moved(sl.from + f * (sl.to - sl.from)) }
                onPressed: (m) => upd(m.x)
                onPositionChanged: (m) => upd(m.x)
                onDoubleClicked: sl.moved(sl.defaultVal)
            }
        }
        Text { text: sl.value.toFixed(2); color: Theme.textDim; font.pixelSize: 9; font.family: Theme.mono; Layout.preferredWidth: 30 }
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

                // ---- Título (solo para clips de título) ----
                ColumnLayout {
                    visible: TimelineModel.selIsTitle
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 9
                    Text { text: "Título"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    // Texto editable
                    Rectangle {
                        Layout.fillWidth: true; height: 30; radius: 4; color: Theme.sunken
                        border.color: tin.activeFocus ? Theme.amber : Theme.line; border.width: 1
                        TextInput {
                            id: tin; anchors.fill: parent; anchors.margins: 7; verticalAlignment: Text.AlignVCenter
                            color: Theme.textHi; font.pixelSize: 12; font.family: Theme.sans; clip: true; selectByMouse: true
                            text: TimelineModel.selTitleText
                            onTextEdited: TimelineModel.setSelTitleText(text)
                        }
                    }
                    NumRow { label: "Tamaño"; value: TimelineModel.selTitleSize; sensitivity: 0.002
                             display: (TimelineModel.selTitleSize * 100).toFixed(1) + " %"
                             onEdited: (v) => TimelineModel.setSelTitleSize(v) }
                    // Alineación
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        Text { text: "Alinear"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; Layout.preferredWidth: 64 }
                        Repeater {
                            model: [ { l: "◧", v: 0 }, { l: "▣", v: 1 }, { l: "◨", v: 2 } ]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true; height: 24; radius: 4
                                readonly property bool on: TimelineModel.selTitleAlign === modelData.v
                                color: on ? Theme.amber : Theme.sunken; border.color: Theme.line; border.width: 1
                                Text { anchors.centerIn: parent; text: modelData.l; font.pixelSize: 13; color: parent.on ? Theme.amberInk : Theme.textMid }
                                TapHandler { onTapped: TimelineModel.setSelTitleAlign(modelData.v) }
                            }
                        }
                    }
                    // Color
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        Text { text: "Color"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; Layout.preferredWidth: 64 }
                        Repeater {
                            model: ["#ffffff", "#101216", "#e2a24b", "#5b8dd6", "#5fbf87"]
                            delegate: Rectangle {
                                required property string modelData
                                width: 22; height: 22; radius: 4; color: modelData
                                border.color: TimelineModel.selTitleColor === modelData ? Theme.amber : Theme.line2
                                border.width: TimelineModel.selTitleColor === modelData ? 2 : 1
                                TapHandler { onTapped: TimelineModel.setSelTitleColor(modelData) }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    // Barra (lower third)
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        Text { text: "Barra (lower third)"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 40; height: 20; radius: 10; border.color: Theme.line; border.width: 1
                            color: TimelineModel.selTitleBar ? Theme.amber : Theme.sunken
                            Rectangle { width: 16; height: 16; radius: 8; color: Theme.textHi; y: 2
                                x: TimelineModel.selTitleBar ? 22 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                            TapHandler { onTapped: TimelineModel.setSelTitleBar(!TimelineModel.selTitleBar) } }
                    }
                }
                Rectangle { visible: TimelineModel.selIsTitle; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

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

                    NumRow { label: "Posición X"; prop: "posX"; value: TimelineModel.selPosX; sensitivity: 0.002
                             display: (TimelineModel.playheadUs, (TimelineModel.selPosX * 1280).toFixed(0) + " px")
                             onEdited: (v) => TimelineModel.setSelPosX(v) }
                    NumRow { label: "Posición Y"; prop: "posY"; value: TimelineModel.selPosY; sensitivity: 0.002
                             display: (TimelineModel.playheadUs, (TimelineModel.selPosY * 720).toFixed(0) + " px")
                             onEdited: (v) => TimelineModel.setSelPosY(v) }
                    NumRow { label: "Escala"; prop: "scale"; value: TimelineModel.selScale; sensitivity: 0.01
                             display: (TimelineModel.playheadUs, (TimelineModel.selScale * 100).toFixed(1) + " %")
                             onEdited: (v) => TimelineModel.setSelScale(v) }
                    NumRow { label: "Rotación"; prop: "rotation"; value: TimelineModel.selRotation; sensitivity: 0.5
                             display: (TimelineModel.playheadUs, TimelineModel.selRotation.toFixed(1) + "°")
                             onEdited: (v) => TimelineModel.setSelRotation(v) }

                    // Opacidad (deslizador) con toggle de keyframe
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        readonly property bool opAnimated: { TimelineModel.hasSelection; return TimelineModel.isKeyframed("opacity") }
                        readonly property bool opKfHere: { TimelineModel.hasSelection; TimelineModel.playheadUs; return TimelineModel.hasKeyframeAtPlayhead("opacity") }
                        Rectangle { width: 8; height: 8; rotation: 45
                            color: parent.opAnimated && parent.opKfHere ? Theme.amber : "transparent"
                            border.color: parent.opAnimated ? Theme.amber : Theme.textFaint; border.width: 1.5
                            TapHandler { onTapped: TimelineModel.toggleKeyframe("opacity") } }
                        Text { text: "Opacidad"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans; Layout.preferredWidth: 64 }
                        Rectangle { id: opTrack; Layout.fillWidth: true; height: 6; radius: 3; color: Theme.sunken
                            readonly property real op: (TimelineModel.playheadUs, TimelineModel.selOpacity)
                            Rectangle { height: parent.height; radius: 3; color: "#4a4d55"; width: parent.width * opTrack.op }
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.text; x: parent.width * opTrack.op - 6; y: -3 }
                            MouseArea { anchors.fill: parent; anchors.margins: -5
                                onPressed: (m) => TimelineModel.setSelOpacity(m.x / opTrack.width)
                                onPositionChanged: (m) => TimelineModel.setSelOpacity(m.x / opTrack.width) } }
                        Text { text: ((TimelineModel.playheadUs, TimelineModel.selOpacity) * 100).toFixed(0) + "%"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; Layout.preferredWidth: 30 }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Velocidad ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 8
                    enabled: TimelineModel.hasSelection
                    opacity: TimelineModel.hasSelection ? 1.0 : 0.4
                    Text { text: "Velocidad · Remapeo"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    // Arrastra el campo para cambiar la velocidad (doble clic = 100 %).
                    NumRow {
                        label: "Velocidad"; value: TimelineModel.selSpeed; sensitivity: 0.01
                        display: (TimelineModel.selSpeed * 100).toFixed(0) + " %"
                        onEdited: (v) => TimelineModel.setSelSpeed(v)
                    }
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: [ { l: "50 %", v: 0.5 }, { l: "100 %", v: 1.0 }, { l: "200 %", v: 2.0 } ]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true; height: 22; radius: 4
                                readonly property bool on: Math.abs(TimelineModel.selSpeed - modelData.v) < 0.005
                                color: on ? Theme.amber : Theme.sunken; border.color: Theme.line; border.width: 1
                                Text { anchors.centerIn: parent; text: modelData.l; font.pixelSize: 10; font.family: Theme.mono
                                       color: parent.on ? Theme.amberInk : Theme.textMid }
                                TapHandler { onTapped: TimelineModel.setSelSpeed(modelData.v) }
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Audio ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 8
                    enabled: TimelineModel.selHasAudio
                    opacity: TimelineModel.selHasAudio ? 1.0 : 0.4
                    RowLayout { Layout.fillWidth: true
                        Text { text: "Audio"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 26; height: 16; radius: 3
                            color: TimelineModel.selAudioMute ? Theme.amber : Theme.sunken; border.color: Theme.line; border.width: 1
                            Text { anchors.centerIn: parent; text: "M"; font.pixelSize: 9; font.family: Theme.mono
                                   color: TimelineModel.selAudioMute ? Theme.amberInk : Theme.textMid }
                            TapHandler { onTapped: TimelineModel.setSelAudioMute(!TimelineModel.selAudioMute) }
                        }
                    }
                    // Ganancia con automatización por keyframes (diamante). Doble unidad = 0 dB.
                    NumRow {
                        label: "Ganancia"; prop: "audioGain"; value: TimelineModel.selAudioGain; sensitivity: 0.01
                        display: TimelineModel.selAudioGain > 0.0001
                                 ? (20*Math.log10(TimelineModel.selAudioGain)).toFixed(1) + " dB" : "−∞ dB"
                        onEdited: (v) => TimelineModel.setSelAudioGain(v)
                    }
                    CSlider { label: "Pan"; from: -1; to: 1; defaultVal: 0; prop: "audioPan"; value: TimelineModel.selPan
                              onMoved: (v) => TimelineModel.setSelPan(v) }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Corrección primaria ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 11
                    enabled: TimelineModel.hasSelection
                    opacity: TimelineModel.hasSelection ? 1.0 : 0.4
                    RowLayout { Layout.fillWidth: true
                        Text { text: "Corrección primaria"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Text { text: "Restablecer"; color: rstHover.hovered ? Theme.amber : Theme.textDim; font.pixelSize: 9; font.family: Theme.sans
                               HoverHandler { id: rstHover }
                               TapHandler { onTapped: TimelineModel.resetSelColor() } }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        ColorWheel { label: "Sombras"; vx: TimelineModel.selLiftX; vy: TimelineModel.selLiftY
                                     onMoved: (x, y) => TimelineModel.setSelLift(x, y) }
                        ColorWheel { label: "Medios"; vx: TimelineModel.selGammaX; vy: TimelineModel.selGammaY
                                     onMoved: (x, y) => TimelineModel.setSelGamma(x, y) }
                        ColorWheel { label: "Altas"; vx: TimelineModel.selGainX; vy: TimelineModel.selGainY
                                     onMoved: (x, y) => TimelineModel.setSelGain(x, y) }
                    }
                    CSlider { label: "Temp"; from: -1; to: 1; defaultVal: 0; value: TimelineModel.selTemp; onMoved: (v) => TimelineModel.setSelTemp(v) }
                    CSlider { label: "Tinte"; from: -1; to: 1; defaultVal: 0; value: TimelineModel.selTint; onMoved: (v) => TimelineModel.setSelTint(v) }
                    CSlider { label: "Saturac."; from: 0; to: 2; defaultVal: 1; value: TimelineModel.selSat; onMoved: (v) => TimelineModel.setSelSat(v) }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Scopes (reales, del fotograma del PROGRAMA) ----
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 9
                    Text { text: "Scopes"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    // Waveform
                    Rectangle {
                        Layout.fillWidth: true; height: 72; radius: 4; color: "#0b0d10"; border.color: Theme.line; border.width: 1; clip: true
                        ScopeView { anchors.fill: parent; anchors.margins: 1; provider: Scopes; kind: "waveform" }
                        Text { text: "WAVEFORM"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono; x: 6; y: 4 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        // Vectorscopio
                        Rectangle {
                            Layout.fillWidth: true; height: 96; radius: 4; color: "#0b0d10"; border.color: Theme.line; border.width: 1; clip: true
                            ScopeView { anchors.centerIn: parent; width: Math.min(parent.width, parent.height); height: width; provider: Scopes; kind: "vectorscope" }
                            Rectangle { anchors.centerIn: parent; width: Math.min(parent.width, parent.height) - 4; height: width; radius: width/2; color: "transparent"; border.color: "#1affffff"; border.width: 1 }
                            Text { text: "VECTOR"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono; x: 6; y: 4 }
                        }
                        // Histograma RGB
                        Rectangle {
                            Layout.fillWidth: true; height: 96; radius: 4; color: "#0b0d10"; border.color: Theme.line; border.width: 1; clip: true
                            Text { text: "HISTOGRAM"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono; x: 6; y: 4 }
                            Canvas {
                                id: histCanvas; anchors.fill: parent; anchors.margins: 4
                                readonly property var data: Scopes.histogram
                                onDataChanged: requestPaint()
                                onPaint: {
                                    var c = getContext("2d"); c.clearRect(0, 0, width, height)
                                    var h = data; if (!h || h.length === 0) return
                                    var n = h.length, bw = width / n
                                    var cols = ["#e26b6b", "#6bd68a", "#6b9ed6"]
                                    c.globalCompositeOperation = "lighter"
                                    for (var ch = 0; ch < 3; ch++) {
                                        c.beginPath(); c.moveTo(0, height)
                                        for (var i = 0; i < n; i++) c.lineTo(i * bw, height - h[i][ch] * height)
                                        c.lineTo(width, height); c.closePath()
                                        c.fillStyle = cols[ch] + "66"; c.fill()
                                        c.strokeStyle = cols[ch]; c.lineWidth = 1; c.stroke()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
