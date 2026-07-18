import QtQuick
import QtQuick.Layouts
import PepeVideo

// Panel derecho: Inspector + Scopes + Color.
Rectangle {
    id: root
    width: 322
    color: Theme.panel

    // Tab activa (0 Inspector · 1 Scopes · 2 Color). Los workspaces del TopBar
    // la fijan al entrar en Fusión/Color.
    property alias currentTab: inspTabs.current

    // Los Scopes analizan el fotograma compuesto por CPU; ese fotograma solo se
    // produce mientras la tab Scopes está visible (el display se compone en GPU).
    Binding {
        target: Compositor
        property: "analysisActive"
        value: root.visible && inspTabs.current === 1
    }

    // Botón de interpolación del keyframe del playhead: L (lineal) → H (hold) →
    // S (suave/smoothstep) → L. Visible solo cuando hay un keyframe en el playhead.
    component KfInterp: Rectangle {
        id: ki
        property string prop
        property bool show: false
        // La automatización de AUDIO se hornea siempre lineal (el motor no ve el
        // interp), así que ahí no se ofrece el selector.
        visible: show && prop.indexOf("audio") !== 0
        width: 14; height: 14; radius: 3
        color: kiHover.hovered ? Theme.hover : Theme.sunken
        border.color: Theme.line; border.width: 1
        readonly property int interp: {
            TimelineModel.hasSelection; TimelineModel.playheadUs
            return ki.prop !== "" ? TimelineModel.keyframeInterpAtPlayhead(ki.prop) : -1
        }
        HoverHandler { id: kiHover }
        TapHandler { onTapped: TimelineModel.cycleKeyframeInterp(ki.prop) }
        Text { anchors.centerIn: parent
               text: ki.interp === 1 ? "H" : ki.interp === 2 ? "S" : "L"
               font.pixelSize: 8; font.family: Theme.mono
               color: ki.interp > 0 ? Theme.amber : Theme.textMid }
    }

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
        KfInterp { prop: nr.prop; show: nr.animated && nr.kfHere }
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
    // Doble clic = centrar (neutro). El diamante anima la pareja X/Y de la rueda.
    component ColorWheel: ColumnLayout {
        id: cw
        property string label
        property string prop: ""
        property real vx: 0
        property real vy: 0
        signal moved(real x, real y)
        readonly property bool animated: { TimelineModel.hasSelection; return cw.prop !== "" && TimelineModel.isKeyframed(cw.prop) }
        readonly property bool kfHere: { TimelineModel.hasSelection; TimelineModel.playheadUs; return cw.prop !== "" && TimelineModel.hasKeyframeAtPlayhead(cw.prop) }
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
        Row {
            Layout.alignment: Qt.AlignHCenter; spacing: 4
            Rectangle {
                visible: cw.prop !== ""; width: 8; height: 8; rotation: 45
                anchors.verticalCenter: parent.verticalCenter
                color: cw.animated && cw.kfHere ? Theme.amber : "transparent"
                border.color: cw.animated ? Theme.amber : Theme.textFaint; border.width: 1.5
                TapHandler { onTapped: TimelineModel.toggleKeyframe(cw.prop) }
            }
            Text { text: cw.label; color: Theme.textMid; font.pixelSize: 9; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
            KfInterp { prop: cw.prop; show: cw.animated && cw.kfHere; anchors.verticalCenter: parent.verticalCenter }
        }
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
        KfInterp { prop: sl.prop; show: sl.animated && sl.kfHere }
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
                id: inspTabs
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
                    visible: inspTabs.current === 0 && TimelineModel.selIsTitle
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
                Rectangle { visible: inspTabs.current === 0 && TimelineModel.selIsTitle; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Transformar ----
                ColumnLayout {
                    visible: inspTabs.current === 0
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
                        KfInterp { prop: "opacity"; show: parent.opAnimated && parent.opKfHere }
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
                Rectangle { visible: inspTabs.current === 0; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Velocidad ----
                ColumnLayout {
                    visible: inspTabs.current === 0
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
                Rectangle { visible: inspTabs.current === 0; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Audio ----
                ColumnLayout {
                    visible: inspTabs.current === 0
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

                    // Efectos por clip: EQ de 3 bandas
                    RowLayout { Layout.fillWidth: true; Layout.topMargin: 2
                        Text { text: "EQ (por clip)"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 34; height: 16; radius: 8; border.color: Theme.line; border.width: 1
                            color: TimelineModel.selAudioEqOn ? Theme.amber : Theme.sunken
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.textHi; y: 2
                                x: TimelineModel.selAudioEqOn ? 20 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                            TapHandler { onTapped: TimelineModel.setSelAudioEqEnabled(!TimelineModel.selAudioEqOn) } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        opacity: TimelineModel.selAudioEqOn ? 1.0 : 0.4; enabled: TimelineModel.selAudioEqOn
                        CSlider { label: "Graves"; from: -18; to: 18; defaultVal: 0; prop: "clipEqLow"; value: TimelineModel.selAudioEqLowDb
                                  onMoved: (v) => TimelineModel.setSelAudioEq(v, TimelineModel.selAudioEqMidDb, TimelineModel.selAudioEqHighDb) }
                        CSlider { label: "Medios"; from: -18; to: 18; defaultVal: 0; prop: "clipEqMid"; value: TimelineModel.selAudioEqMidDb
                                  onMoved: (v) => TimelineModel.setSelAudioEq(TimelineModel.selAudioEqLowDb, v, TimelineModel.selAudioEqHighDb) }
                        CSlider { label: "Agudos"; from: -18; to: 18; defaultVal: 0; prop: "clipEqHigh"; value: TimelineModel.selAudioEqHighDb
                                  onMoved: (v) => TimelineModel.setSelAudioEq(TimelineModel.selAudioEqLowDb, TimelineModel.selAudioEqMidDb, v) }
                    }

                    // Efectos por clip: compresor
                    RowLayout { Layout.fillWidth: true; Layout.topMargin: 2
                        Text { text: "Compresor (por clip)"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 34; height: 16; radius: 8; border.color: Theme.line; border.width: 1
                            color: TimelineModel.selAudioCompOn ? Theme.amber : Theme.sunken
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.textHi; y: 2
                                x: TimelineModel.selAudioCompOn ? 20 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                            TapHandler { onTapped: TimelineModel.setSelAudioCompEnabled(!TimelineModel.selAudioCompOn) } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        opacity: TimelineModel.selAudioCompOn ? 1.0 : 0.4; enabled: TimelineModel.selAudioCompOn
                        CSlider { label: "Umbral"; from: -48; to: 0; defaultVal: -18; value: TimelineModel.selAudioCompThreshDb
                                  onMoved: (v) => TimelineModel.setSelAudioComp(v, TimelineModel.selAudioCompRatio, TimelineModel.selAudioCompMakeupDb) }
                        CSlider { label: "Ratio"; from: 1; to: 20; defaultVal: 2; value: TimelineModel.selAudioCompRatio
                                  onMoved: (v) => TimelineModel.setSelAudioComp(TimelineModel.selAudioCompThreshDb, v, TimelineModel.selAudioCompMakeupDb) }
                        CSlider { label: "Makeup"; from: 0; to: 24; defaultVal: 0; value: TimelineModel.selAudioCompMakeupDb
                                  onMoved: (v) => TimelineModel.setSelAudioComp(TimelineModel.selAudioCompThreshDb, TimelineModel.selAudioCompRatio, v) }
                    }

                    // Puerta de ruido (por clip)
                    RowLayout { Layout.fillWidth: true; Layout.topMargin: 2
                        Text { text: "Puerta (por clip)"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 34; height: 16; radius: 8; border.color: Theme.line; border.width: 1
                            color: TimelineModel.selAudioGateOn ? Theme.amber : Theme.sunken
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.textHi; y: 2
                                x: TimelineModel.selAudioGateOn ? 20 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                            TapHandler { onTapped: TimelineModel.setSelAudioGateEnabled(!TimelineModel.selAudioGateOn) } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        opacity: TimelineModel.selAudioGateOn ? 1.0 : 0.4; enabled: TimelineModel.selAudioGateOn
                        CSlider { label: "Umbral"; from: -80; to: 0; defaultVal: -40; value: TimelineModel.selAudioGateThreshDb
                                  onMoved: (v) => TimelineModel.setSelAudioGate(v) }
                    }

                    // De-esser (por clip)
                    RowLayout { Layout.fillWidth: true; Layout.topMargin: 2
                        Text { text: "De-esser (por clip)"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 34; height: 16; radius: 8; border.color: Theme.line; border.width: 1
                            color: TimelineModel.selAudioDeEssOn ? Theme.amber : Theme.sunken
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.textHi; y: 2
                                x: TimelineModel.selAudioDeEssOn ? 20 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                            TapHandler { onTapped: TimelineModel.setSelAudioDeEsserEnabled(!TimelineModel.selAudioDeEssOn) } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        opacity: TimelineModel.selAudioDeEssOn ? 1.0 : 0.4; enabled: TimelineModel.selAudioDeEssOn
                        CSlider { label: "Umbral"; from: -48; to: 0; defaultVal: -24; value: TimelineModel.selAudioDeEssThreshDb
                                  onMoved: (v) => TimelineModel.setSelAudioDeEsser(v) }
                    }

                    // Reverb (por clip)
                    RowLayout { Layout.fillWidth: true; Layout.topMargin: 2
                        Text { text: "Reverb (por clip)"; color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 34; height: 16; radius: 8; border.color: Theme.line; border.width: 1
                            color: TimelineModel.selAudioReverbOn ? Theme.amber : Theme.sunken
                            Rectangle { width: 12; height: 12; radius: 6; color: Theme.textHi; y: 2
                                x: TimelineModel.selAudioReverbOn ? 20 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                            TapHandler { onTapped: TimelineModel.setSelAudioReverbEnabled(!TimelineModel.selAudioReverbOn) } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        opacity: TimelineModel.selAudioReverbOn ? 1.0 : 0.4; enabled: TimelineModel.selAudioReverbOn
                        CSlider { label: "Mezcla"; from: 0; to: 1; defaultVal: 0.25; prop: "clipReverbMix"; value: TimelineModel.selAudioReverbMix
                                  onMoved: (v) => TimelineModel.setSelAudioReverb(v, TimelineModel.selAudioReverbSize) }
                        CSlider { label: "Tamaño"; from: 0; to: 1; defaultVal: 0.5; value: TimelineModel.selAudioReverbSize
                                  onMoved: (v) => TimelineModel.setSelAudioReverb(TimelineModel.selAudioReverbMix, v) }
                    }
                }
                Rectangle { visible: inspTabs.current === 0; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Curvas (editor de keyframes de las propiedades animadas) ----
                ColumnLayout {
                    id: curveEd
                    visible: inspTabs.current === 0 && TimelineModel.hasSelection && animatedProps.length > 0
                    Layout.fillWidth: true; Layout.margins: 12; spacing: 8
                    // Propiedades escalares que el editor sabe dibujar, con su rango.
                    readonly property var allProps: [
                        { p: "opacity", l: "Opacidad", min: 0, max: 1 },
                        { p: "posX", l: "Pos X", min: -1, max: 1 },
                        { p: "posY", l: "Pos Y", min: -1, max: 1 },
                        { p: "scale", l: "Escala", min: 0, max: 4 },
                        { p: "rotation", l: "Rotación", min: -180, max: 180 },
                        { p: "temp", l: "Temp", min: -1, max: 1 },
                        { p: "tint", l: "Tinte", min: -1, max: 1 },
                        { p: "sat", l: "Saturac.", min: 0, max: 2 },
                        { p: "audioGain", l: "Gan. audio", min: 0, max: 2 },
                        { p: "audioPan", l: "Pan", min: -1, max: 1 },
                        { p: "clipEqLow", l: "EQ Graves", min: -18, max: 18 },
                        { p: "clipEqMid", l: "EQ Medios", min: -18, max: 18 },
                        { p: "clipEqHigh", l: "EQ Agudos", min: -18, max: 18 },
                        { p: "clipReverbMix", l: "Reverb mix", min: 0, max: 1 } ]
                    readonly property var animatedProps: {
                        TimelineModel.hasSelection
                        return allProps.filter(e => TimelineModel.isKeyframed(e.p))
                    }
                    property int currentIdx: 0
                    // Cambiar de propiedad devuelve el lienzo a la vista completa.
                    onCurrentIdxChanged: { curveCanvas.viewZ = 1; curveCanvas.viewX0 = 0 }
                    readonly property var current: animatedProps.length > 0
                        ? animatedProps[Math.min(currentIdx, animatedProps.length - 1)] : null
                    RowLayout { Layout.fillWidth: true
                        Text { text: "Curvas"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                        Item { Layout.fillWidth: true }
                        // Con zoom, un chip muestra el factor; clic = volver a la vista completa.
                        Rectangle {
                            visible: curveCanvas.viewZ > 1.001
                            width: zoomT.width + 10; height: 14; radius: 3
                            color: Theme.sunken; border.color: Theme.line; border.width: 1
                            Text { id: zoomT; anchors.centerIn: parent; color: Theme.textMid
                                   text: "×" + curveCanvas.viewZ.toFixed(1); font.pixelSize: 8; font.family: Theme.sans }
                            TapHandler { onTapped: { curveCanvas.viewZ = 1; curveCanvas.viewX0 = 0 } }
                        }
                        Text { text: "doble clic = añadir/quitar · rueda = zoom · fondo = paneo"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.sans }
                    }
                    // Selector de propiedad (solo las animadas)
                    Flow {
                        Layout.fillWidth: true; spacing: 4
                        Repeater {
                            model: curveEd.animatedProps
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                readonly property bool on: curveEd.current && curveEd.current.p === modelData.p
                                width: chipT.width + 14; height: 20; radius: 4
                                color: on ? Theme.amber : Theme.sunken; border.color: Theme.line; border.width: 1
                                Text { id: chipT; anchors.centerIn: parent; text: modelData.l; font.pixelSize: 9; font.family: Theme.sans
                                       color: parent.on ? Theme.amberInk : Theme.textMid }
                                TapHandler { onTapped: curveEd.currentIdx = index }
                            }
                        }
                    }
                    // Lienzo de la curva: keyframes (cuadrados) e interpolación real.
                    Rectangle {
                        Layout.fillWidth: true; height: 96; radius: 4
                        color: "#0b0d10"; border.color: Theme.line; border.width: 1; clip: true
                        Canvas {
                            id: curveCanvas
                            anchors.fill: parent; anchors.margins: 6
                            // Ventana visible del eje X: [viewX0, viewX0 + 1/viewZ] en fracción del clip.
                            property real viewZ: 1
                            property real viewX0: 0
                            onViewZChanged: requestPaint()
                            onViewX0Changed: requestPaint()
                            readonly property var pts: {
                                TimelineModel.hasSelection
                                return curveEd.current ? TimelineModel.keyframePoints(curveEd.current.p) : []
                            }
                            onPtsChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            function toX(fx)  { return (fx - viewX0) * viewZ * width }
                            function fromX(px) { return viewX0 + px / (viewZ * width) }
                            function toY(v) {
                                var c = curveEd.current
                                var f = (v - c.min) / (c.max - c.min)
                                return Math.max(0, Math.min(height, height - f * height))
                            }
                            // Réplica JS de evalKf (lineal / hold / suave) para dibujar.
                            function evalAt(fx) {
                                var p = pts, c = curveEd.current
                                if (p.length === 0) return c.min
                                if (fx <= p[0].x) return p[0].v
                                if (fx >= p[p.length - 1].x) return p[p.length - 1].v
                                for (var i = 1; i < p.length; i++) {
                                    if (fx <= p[i].x) {
                                        var a = p[i - 1], b = p[i]
                                        if (a.interp === 1) return a.v
                                        var span = b.x - a.x
                                        var t = span > 0 ? (fx - a.x) / span : 0
                                        if (a.interp === 2) t = t * t * (3 - 2 * t)
                                        return a.v + (b.v - a.v) * t
                                    }
                                }
                                return p[p.length - 1].v
                            }
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                if (!curveEd.current) return
                                ctx.strokeStyle = "#1effffff"; ctx.lineWidth = 1
                                for (var g = 1; g < 4; g++) {
                                    ctx.beginPath(); ctx.moveTo(0, height * g / 4)
                                    ctx.lineTo(width, height * g / 4); ctx.stroke()
                                }
                                ctx.strokeStyle = Theme.amber; ctx.lineWidth = 1.5
                                ctx.beginPath()
                                for (var x = 0; x <= width; x += 2) {
                                    var y = toY(evalAt(fromX(x)))
                                    if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                                }
                                ctx.stroke()
                                for (var i = 0; i < pts.length; i++) {
                                    var px = toX(pts[i].x), py = toY(pts[i].v)
                                    if (px < -3 || px > width + 3) continue
                                    ctx.fillStyle = pts[i].interp === 1 ? "#5b8dd6"
                                                  : pts[i].interp === 2 ? "#4a9e6b" : Theme.amber
                                    ctx.fillRect(px - 3, py - 3, 6, 6)
                                }
                            }
                        }
                        MouseArea {
                            anchors.fill: curveCanvas
                            anchors.margins: -4
                            property int dragIdx: -1
                            property real panPx: -1       // último x del paneo de fondo (-1 = inactivo)
                            function nearest(mx, my) {
                                var p = curveCanvas.pts, best = -1, bd = 14
                                for (var i = 0; i < p.length; i++) {
                                    var d = Math.hypot(mx - curveCanvas.toX(p[i].x),
                                                       my - curveCanvas.toY(p[i].v))
                                    if (d < bd) { bd = d; best = i }
                                }
                                return best
                            }
                            function clampView() {
                                var span = 1 / curveCanvas.viewZ
                                curveCanvas.viewX0 = Math.max(0, Math.min(1 - span, curveCanvas.viewX0))
                            }
                            onPressed: (m) => {
                                dragIdx = nearest(m.x - 4, m.y - 4)
                                panPx = dragIdx < 0 ? m.x : -1
                            }
                            onPositionChanged: (m) => {
                                if (dragIdx >= 0 && curveEd.current) {
                                    var c = curveEd.current
                                    var fx = Math.max(0, Math.min(1, curveCanvas.fromX(m.x - 4)))
                                    var fy = Math.max(0, Math.min(1, (m.y - 4) / curveCanvas.height))
                                    TimelineModel.moveKeyframePoint(c.p, dragIdx, fx, c.min + (1 - fy) * (c.max - c.min))
                                    // Reordenar puede cambiar el índice: re-identifica bajo el cursor.
                                    dragIdx = nearest(m.x - 4, m.y - 4)
                                } else if (panPx >= 0) {
                                    curveCanvas.viewX0 -= (m.x - panPx) / (curveCanvas.viewZ * curveCanvas.width)
                                    panPx = m.x
                                    clampView()
                                }
                            }
                            onReleased: { dragIdx = -1; panPx = -1 }
                            onDoubleClicked: (m) => {
                                if (!curveEd.current) return
                                var c = curveEd.current
                                var i = nearest(m.x - 4, m.y - 4)
                                if (i >= 0) { TimelineModel.removeKeyframePoint(c.p, i); return }
                                // Doble clic en el fondo: crea un keyframe en ese tiempo/valor.
                                var fx = Math.max(0, Math.min(1, curveCanvas.fromX(m.x - 4)))
                                var fy = Math.max(0, Math.min(1, (m.y - 4) / curveCanvas.height))
                                TimelineModel.addKeyframePoint(c.p, fx, c.min + (1 - fy) * (c.max - c.min))
                            }
                            // Rueda: zoom horizontal alrededor del cursor (×1 … ×20).
                            onWheel: (w) => {
                                var f = Math.pow(1.25, w.angleDelta.y / 120)
                                var z = Math.max(1, Math.min(20, curveCanvas.viewZ * f))
                                if (z === curveCanvas.viewZ) return
                                var fx = curveCanvas.fromX(w.x - 4)   // fracción bajo el cursor
                                curveCanvas.viewZ = z
                                curveCanvas.viewX0 = fx - (w.x - 4) / (z * curveCanvas.width)
                                clampView()
                            }
                        }
                    }
                    // Leyenda de interpolación (colores de los puntos)
                    Row { spacing: 10
                        Repeater {
                            model: [{ c: Theme.amber, t: "lineal" }, { c: "#5b8dd6", t: "hold" }, { c: "#4a9e6b", t: "suave" }]
                            delegate: Row {
                                required property var modelData
                                spacing: 3
                                Rectangle { width: 6; height: 6; color: modelData.c; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: modelData.t; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                            }
                        }
                    }
                }
                Rectangle { visible: curveEd.visible; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Corrección primaria ----
                ColumnLayout {
                    visible: inspTabs.current === 2
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
                        ColorWheel { label: "Sombras"; prop: "lift"
                                     vx: (TimelineModel.playheadUs, TimelineModel.selLiftX)
                                     vy: (TimelineModel.playheadUs, TimelineModel.selLiftY)
                                     onMoved: (x, y) => TimelineModel.setSelLift(x, y) }
                        ColorWheel { label: "Medios"; prop: "gamma"
                                     vx: (TimelineModel.playheadUs, TimelineModel.selGammaX)
                                     vy: (TimelineModel.playheadUs, TimelineModel.selGammaY)
                                     onMoved: (x, y) => TimelineModel.setSelGamma(x, y) }
                        ColorWheel { label: "Altas"; prop: "gain"
                                     vx: (TimelineModel.playheadUs, TimelineModel.selGainX)
                                     vy: (TimelineModel.playheadUs, TimelineModel.selGainY)
                                     onMoved: (x, y) => TimelineModel.setSelGain(x, y) }
                    }
                    // temp/tint/sat son animables (diamante + interpolación); el playhead
                    // en el paréntesis fuerza la reevaluación al moverse.
                    CSlider { label: "Temp"; from: -1; to: 1; defaultVal: 0; prop: "temp"
                              value: (TimelineModel.playheadUs, TimelineModel.selTemp); onMoved: (v) => TimelineModel.setSelTemp(v) }
                    CSlider { label: "Tinte"; from: -1; to: 1; defaultVal: 0; prop: "tint"
                              value: (TimelineModel.playheadUs, TimelineModel.selTint); onMoved: (v) => TimelineModel.setSelTint(v) }
                    CSlider { label: "Saturac."; from: 0; to: 2; defaultVal: 1; prop: "sat"
                              value: (TimelineModel.playheadUs, TimelineModel.selSat); onMoved: (v) => TimelineModel.setSelSat(v) }
                }
                Rectangle { visible: inspTabs.current === 2; Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                // ---- Scopes (reales, del fotograma del PROGRAMA) ----
                ColumnLayout {
                    visible: inspTabs.current === 1
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
                                // No usar el nombre `data`: es la propiedad por defecto de Item
                                // (lista de hijos) y sombrearla genera un aviso de propertyCache.
                                readonly property var histData: Scopes.histogram
                                onHistDataChanged: requestPaint()
                                onPaint: {
                                    var c = getContext("2d"); c.clearRect(0, 0, width, height)
                                    var h = histData; if (!h || h.length === 0) return
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
