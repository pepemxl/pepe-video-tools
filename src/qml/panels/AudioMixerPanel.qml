import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import PepeVideo

// Página «Audio»: consola de mezcla dedicada (tira de canal por pista de audio +
// bus MAIN). Reutiliza el modelo del timeline: setTrackGain/Pan/Mute/Solo y el
// master (setMasterGain/Pan). Visible solo en el workspace Audio (modo 4).
Rectangle {
    id: root
    color: Theme.bg1

    // Canales: una entrada por pista de audio real + MAIN. `trk` = índice global
    // de pista (casa con setTrackGain/audioTracks[trk]); -1 = MAIN.
    readonly property var channels: {
        var out = []
        var palette = [Theme.green, Theme.purple, Theme.blue]
        var ats = TimelineModel.audioTracks
        var k = 0
        for (var i = 0; i < ats.length; i++) {
            if (ats[i].kind !== "audio") continue
            out.push({ id: ats[i].name, col: palette[k % palette.length], trk: ats[i].index, main: false })
            k++
        }
        out.push({ id: "MAIN", col: Theme.amber, trk: -1, main: true })
        return out
    }

    // Deslizador horizontal para el editor de efectos (etiqueta · barra · valor).
    component FxSlider: RowLayout {
        id: fsl
        property string label
        property real value: 0
        property real from: -18
        property real to: 18
        property real defVal: 0
        property string unit: "dB"
        property int decimals: 0
        signal moved(real v)
        Layout.fillWidth: true; spacing: 8
        Text { text: fsl.label; color: Theme.textMid; font.pixelSize: 10; font.family: Theme.sans; Layout.preferredWidth: 52 }
        Rectangle { id: fslTr; Layout.fillWidth: true; height: 5; radius: 3; color: Theme.sunken
            readonly property real frac: (fsl.value - fsl.from) / (fsl.to - fsl.from)
            Rectangle { width: 11; height: 11; radius: 6; color: fslDrag.pressed ? Theme.amber : Theme.textHi
                        x: fslTr.width * Math.max(0, Math.min(1, fslTr.frac)) - 5.5; y: -3 }
            MouseArea { id: fslDrag; anchors.fill: parent; anchors.margins: -6
                function upd(mx) { var f = Math.max(0, Math.min(1, mx / fslTr.width)); fsl.moved(fsl.from + f * (fsl.to - fsl.from)) }
                onPressed: (m) => upd(m.x); onPositionChanged: (m) => upd(m.x); onDoubleClicked: fsl.moved(fsl.defVal) } }
        Text { text: (fsl.value > 0 && fsl.unit === "dB" ? "+" : "") + fsl.value.toFixed(fsl.decimals) + (fsl.unit !== "" ? " " + fsl.unit : "")
               color: Theme.textDim; font.pixelSize: 9; font.family: Theme.mono; Layout.preferredWidth: 46; horizontalAlignment: Text.AlignRight }
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line3 }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // Cabecera con LUFS
        Rectangle {
            Layout.fillWidth: true; height: 34; color: Theme.panelHead
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            Text { anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                   text: "Audio · Consola de mezcla"; color: Theme.textHi; font.pixelSize: 12; font.weight: Font.DemiBold; font.family: Theme.sans }
            Row {
                anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter; spacing: 16
                Text { text: (Audio.lufs <= -60 ? "−∞" : Audio.lufs.toFixed(1)) + " LUFS-I"
                       color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono; anchors.verticalCenter: parent.verticalCenter }
                Text { text: (Audio.lufsShort <= -60 ? "−∞" : Audio.lufsShort.toFixed(1)) + " LUFS-M"
                       color: Audio.playing ? Theme.amber : Theme.textFaint; font.pixelSize: 10; font.family: Theme.mono; anchors.verticalCenter: parent.verticalCenter }
            }
        }

        // Tiras de canal (con scroll horizontal si hay muchas)
        Flickable {
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: strips.width; contentHeight: height; clip: true
            flickableDirection: Flickable.HorizontalFlick
            Row {
                id: strips
                height: parent.height; spacing: 0
                Repeater {
                    model: root.channels
                    delegate: Rectangle {
                        id: strip
                        required property var modelData
                        required property int index
                        width: 104; height: strips.height
                        color: modelData.main ? "#171922" : (index % 2 ? Theme.bg2 : Theme.panel)
                        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }

                        // Nivel real: MAIN = pico del master; pistas = envolvente.
                        readonly property real lvl: modelData.main
                            ? Math.max(Audio.peakL, Audio.peakR)
                            : (Audio.trackPeaks[modelData.trk] || 0)
                        readonly property var atrack: modelData.trk >= 0 ? TimelineModel.audioTracks[modelData.trk] : null
                        readonly property real chGain: modelData.main ? TimelineModel.masterGain : (atrack ? atrack.gain : 1.0)
                        readonly property real chPan: modelData.main ? TimelineModel.masterPan : (atrack ? atrack.pan : 0.0)
                        readonly property bool chMute: atrack ? atrack.mute : false
                        readonly property bool chSolo: atrack ? atrack.solo : false
                        // Efectos de la pista (para el editor FX).
                        readonly property bool eqOn: atrack ? atrack.eqOn : false
                        readonly property real eqLow: atrack ? atrack.eqLowDb : 0
                        readonly property real eqMid: atrack ? atrack.eqMidDb : 0
                        readonly property real eqHigh: atrack ? atrack.eqHighDb : 0
                        readonly property bool compOn: atrack ? atrack.compOn : false
                        readonly property real compThresh: atrack ? atrack.compThreshDb : -18
                        readonly property real compRatio: atrack ? atrack.compRatio : 2
                        readonly property real compMakeup: atrack ? atrack.compMakeupDb : 0

                        ColumnLayout {
                            anchors.fill: parent; anchors.topMargin: 10; anchors.bottomMargin: 10; spacing: 8

                            // Nombre
                            Text { Layout.alignment: Qt.AlignHCenter; text: strip.modelData.id
                                   color: strip.modelData.col; font.pixelSize: 11; font.weight: Font.Bold; font.family: Theme.mono }

                            // FX (EQ + compresor): abre el editor de efectos de la pista.
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter; visible: !strip.modelData.main
                                width: 46; height: 18; radius: 4
                                readonly property bool active: strip.eqOn || strip.compOn
                                color: fxPop.visible ? Theme.amber : (active ? Qt.rgba(0.88, 0.64, 0.29, 0.22) : Theme.hover2)
                                border.color: active ? Theme.amber : Theme.line; border.width: 1
                                TapHandler { onTapped: fxPop.open() }
                                Text { anchors.centerIn: parent; text: "FX"; font.pixelSize: 9; font.weight: Font.Bold; font.family: Theme.sans
                                       color: fxPop.visible ? Theme.amberInk : (parent.active ? Theme.amber : Theme.textDim) }

                                C.Popup {
                                    id: fxPop
                                    y: parent.height + 4
                                    x: -100
                                    width: 250; padding: 12
                                    background: Rectangle { color: Theme.panel2; border.color: Theme.line; border.width: 1; radius: 8 }
                                    contentItem: ColumnLayout {
                                        spacing: 7
                                        Text { text: strip.modelData.id + " · Efectos"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }

                                        // ---- Ecualizador ----
                                        RowLayout { Layout.fillWidth: true
                                            Text { text: "Ecualizador (3 bandas)"; color: Theme.textMid; font.pixelSize: 10; font.family: Theme.sans }
                                            Item { Layout.fillWidth: true }
                                            Rectangle { width: 34; height: 18; radius: 9; border.color: Theme.line; border.width: 1
                                                color: strip.eqOn ? Theme.amber : Theme.sunken
                                                Rectangle { width: 14; height: 14; radius: 7; color: Theme.textHi; y: 2
                                                    x: strip.eqOn ? 18 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                                                TapHandler { onTapped: TimelineModel.setTrackEqEnabled(strip.modelData.trk, !strip.eqOn) } }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true; spacing: 5
                                            opacity: strip.eqOn ? 1.0 : 0.4; enabled: strip.eqOn
                                            FxSlider { label: "Graves"; value: strip.eqLow; from: -18; to: 18; defVal: 0
                                                       onMoved: (v) => TimelineModel.setTrackEq(strip.modelData.trk, v, strip.eqMid, strip.eqHigh) }
                                            FxSlider { label: "Medios"; value: strip.eqMid; from: -18; to: 18; defVal: 0
                                                       onMoved: (v) => TimelineModel.setTrackEq(strip.modelData.trk, strip.eqLow, v, strip.eqHigh) }
                                            FxSlider { label: "Agudos"; value: strip.eqHigh; from: -18; to: 18; defVal: 0
                                                       onMoved: (v) => TimelineModel.setTrackEq(strip.modelData.trk, strip.eqLow, strip.eqMid, v) }
                                        }

                                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.lineSoft }

                                        // ---- Compresor ----
                                        RowLayout { Layout.fillWidth: true
                                            Text { text: "Compresor"; color: Theme.textMid; font.pixelSize: 10; font.family: Theme.sans }
                                            Item { Layout.fillWidth: true }
                                            Rectangle { width: 34; height: 18; radius: 9; border.color: Theme.line; border.width: 1
                                                color: strip.compOn ? Theme.amber : Theme.sunken
                                                Rectangle { width: 14; height: 14; radius: 7; color: Theme.textHi; y: 2
                                                    x: strip.compOn ? 18 : 2; Behavior on x { NumberAnimation { duration: 90 } } }
                                                TapHandler { onTapped: TimelineModel.setTrackCompEnabled(strip.modelData.trk, !strip.compOn) } }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true; spacing: 5
                                            opacity: strip.compOn ? 1.0 : 0.4; enabled: strip.compOn
                                            FxSlider { label: "Umbral"; value: strip.compThresh; from: -48; to: 0; defVal: -18
                                                       onMoved: (v) => TimelineModel.setTrackComp(strip.modelData.trk, v, strip.compRatio, strip.compMakeup) }
                                            FxSlider { label: "Ratio"; value: strip.compRatio; from: 1; to: 20; defVal: 2; unit: "x"; decimals: 1
                                                       onMoved: (v) => TimelineModel.setTrackComp(strip.modelData.trk, strip.compThresh, v, strip.compMakeup) }
                                            FxSlider { label: "Makeup"; value: strip.compMakeup; from: 0; to: 24; defVal: 0
                                                       onMoved: (v) => TimelineModel.setTrackComp(strip.modelData.trk, strip.compThresh, strip.compRatio, v) }
                                        }
                                    }
                                }
                            }

                            // Perilla de pan (arrastra horizontal; doble clic = centro)
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter; width: 30; height: 30; radius: 15
                                color: Theme.hover2; border.color: panKnob.pressed ? Theme.amber : Theme.line2; border.width: 1
                                Rectangle { width: 2; height: 12; radius: 1; color: strip.modelData.main ? Theme.amber : Theme.text
                                    x: parent.width/2 - 1; y: 3; transformOrigin: Item.Bottom; rotation: strip.chPan * 135 }
                                MouseArea {
                                    id: panKnob; anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.SizeHorCursor
                                    property real base; property real px
                                    onPressed: (m) => { base = strip.chPan; px = m.x }
                                    onPositionChanged: (m) => { var v = base + (m.x - px) * 0.015
                                        strip.modelData.main ? TimelineModel.setMasterPan(v) : TimelineModel.setTrackPan(strip.modelData.trk, v) }
                                    onDoubleClicked: strip.modelData.main ? TimelineModel.setMasterPan(0) : TimelineModel.setTrackPan(strip.modelData.trk, 0)
                                }
                            }
                            Text { Layout.alignment: Qt.AlignHCenter
                                   text: Math.abs(strip.chPan) < 0.01 ? "C" : (strip.chPan < 0 ? "I" : "D") + Math.round(Math.abs(strip.chPan)*100)
                                   color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono }

                            // Medidor + fader grande (ocupa el alto restante)
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter; Layout.fillHeight: true; spacing: 6
                                // Medidor
                                Rectangle { width: 12; Layout.fillHeight: true; radius: 3; color: Theme.sunken; clip: true
                                    Rectangle { width: parent.width; anchors.bottom: parent.bottom
                                        height: parent.height * Math.min(1, strip.lvl)
                                        gradient: Gradient {
                                            GradientStop { position: 0.0; color: "#4a9e6b" }
                                            GradientStop { position: 0.6; color: "#4a9e6b" }
                                            GradientStop { position: 0.85; color: "#e2a24b" }
                                            GradientStop { position: 1.0; color: "#c0392b" } }
                                        Behavior on height { NumberAnimation { duration: 70 } } } }
                                // Fader (arrastra vertical; doble clic = 0 dB). Rango 0…+12 dB → gain 0…4.
                                Rectangle { id: faderTrack; width: 14; Layout.fillHeight: true; radius: 7; color: Theme.sunken
                                    // Marca de 0 dB (gain 1.0).
                                    Rectangle { width: parent.width + 6; height: 1; x: -3; color: Theme.line2
                                                y: (1 - 1.0/4) * (parent.height - 14) + 7 }
                                    readonly property real capY: (1 - Math.min(1, strip.chGain / 4)) * (height - 14)
                                    Rectangle { width: 26; height: 14; radius: 3; color: faderDrag.pressed ? Theme.amber : "#3a3d45"
                                        border.color: strip.modelData.main ? Theme.amber : "#55575f"; border.width: 1
                                        x: parent.width/2 - 13; y: faderTrack.capY }
                                    MouseArea {
                                        id: faderDrag; anchors.fill: parent; anchors.margins: -6; cursorShape: Qt.SizeVerCursor
                                        function setFromY(my) { var f = 1 - Math.max(0, Math.min(1, my / faderTrack.height))
                                            strip.modelData.main ? TimelineModel.setMasterGain(f * 4) : TimelineModel.setTrackGain(strip.modelData.trk, f * 4) }
                                        onPressed: (m) => setFromY(m.y)
                                        onPositionChanged: (m) => setFromY(m.y)
                                        onDoubleClicked: strip.modelData.main ? TimelineModel.setMasterGain(1.0) : TimelineModel.setTrackGain(strip.modelData.trk, 1.0)
                                    } }
                            }

                            // dB
                            Text { Layout.alignment: Qt.AlignHCenter
                                   text: strip.chGain > 0.0001 ? (20*Math.log10(strip.chGain)).toFixed(1) + " dB" : "−∞"
                                   color: strip.modelData.main ? Theme.amber : Theme.textMid; font.pixelSize: 9; font.family: Theme.mono }

                            // M / S (no en MAIN)
                            Row { Layout.alignment: Qt.AlignHCenter; spacing: 5; visible: !strip.modelData.main
                                Repeater { model: ["M","S"]
                                    delegate: Rectangle {
                                        required property string modelData
                                        readonly property bool isMute: modelData === "M"
                                        readonly property bool on: isMute ? strip.chMute : strip.chSolo
                                        width: 20; height: 18; radius: 3
                                        color: on ? (isMute ? Theme.amber : Theme.blue) : Theme.hover2
                                        Text { anchors.centerIn: parent; text: parent.modelData; font.pixelSize: 9; font.weight: Font.DemiBold
                                               color: parent.on ? "#101216" : Theme.textDim }
                                        TapHandler {
                                            onTapped: parent.isMute
                                                ? TimelineModel.setTrackMute(strip.modelData.trk, !strip.chMute)
                                                : TimelineModel.setTrackSolo(strip.modelData.trk, !strip.chSolo)
                                        }
                                    }
                                }
                            }
                            // Etiqueta BUS en MAIN (donde irían M/S)
                            Text { Layout.alignment: Qt.AlignHCenter; visible: strip.modelData.main
                                   text: "BUS"; color: Theme.textFaint; font.pixelSize: 8; font.family: Theme.mono }
                        }
                    }
                }
            }
        }
    }
}
