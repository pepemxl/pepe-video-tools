import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import PepeVideo

// Centro: monitores Origen + Programa.
Rectangle {
    color: Theme.bg1

    RowLayout {
        anchors.fill: parent
        spacing: 1

        // ----- Monitor genérico -----
        component Monitor: ColumnLayout {
            id: mon
            property string title
            property color titleColor: Theme.textMid
            property string clipName
            property color screenBg
            property string caption
            property bool program: false
            property real fillW
            property string tcLeft
            property string tcRight
            property color tcLeftColor: Theme.textMid
            property var ctrl: null                         // VideoController o null
            readonly property bool live: ctrl && ctrl.hasMedia
            // El monitor de PROGRAMA muestra el compositor multicapa de la timeline.
            readonly property bool progLive: program && Compositor.hasContent
            readonly property bool showVideo: live || progLive
            // Zoom del visor: 0 = ajustar; >0 = factor sobre el tamaño nativo.
            // Con zoom, arrastrar sobre el vídeo panea (el paneo se resetea al cambiar).
            property real viewerZoom: 0
            property real viewPanX: 0
            property real viewPanY: 0
            onViewerZoomChanged: { viewPanX = 0; viewPanY = 0 }
            readonly property string zoomText: viewerZoom === 0 ? "Ajustar" : Math.round(viewerZoom * 100) + "%"
            spacing: 0

            // Paso de fotograma (◀◀/▶▶): ORIGEN vía el decodificador; PROGRAMA
            // moviendo el playhead un fotograma de la secuencia (pausa primero).
            function stepFrames(n) {
                if (program) {
                    if (Compositor.playing) Compositor.pause()
                    const fps = Project.seqFps > 0 ? Project.seqFps : 30
                    TimelineModel.setPlayheadUs(TimelineModel.playheadUs + n * Math.round(1e6 / fps))
                } else if (live) {
                    ctrl.stepFrame(n)
                }
            }

            // Timecode hh:mm:ss:ff de la secuencia (para el monitor de PROGRAMA).
            function seqTc(usVal) {
                const fps = Project.seqFps > 0 ? Project.seqFps : 30
                const totalFrames = Math.max(0, Math.floor(usVal * fps / 1e6))
                const ff = totalFrames % Math.round(fps)
                const secs = Math.floor(totalFrames / fps)
                const h = Math.floor(secs / 3600)
                const m = Math.floor((secs % 3600) / 60)
                const s = secs % 60
                const p = (n) => (n < 10 ? "0" : "") + n
                return p(h) + ":" + p(m) + ":" + p(s) + ":" + p(ff)
            }

            Rectangle {
                Layout.fillWidth: true; height: 30; color: Theme.panel3
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                    Text { text: title; color: titleColor; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    Item { Layout.fillWidth: true }
                    Text { text: live ? ctrl.sourceName : clipName; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.sans; elide: Text.ElideMiddle; Layout.maximumWidth: 220 }
                    Item { Layout.fillWidth: true }
                    // Zoom del visor: desplegable Ajustar / 50% / 100% / 200%.
                    Rectangle {
                        implicitWidth: zoomTxt.width + 12; implicitHeight: 20; radius: 4
                        color: zoomHover.hovered ? Theme.hover : "transparent"
                        border.color: zoomPop.visible ? Theme.amber : "transparent"; border.width: 1
                        HoverHandler { id: zoomHover }
                        TapHandler { onTapped: zoomPop.open() }
                        Text { id: zoomTxt; anchors.centerIn: parent; text: mon.zoomText + " ▾"
                               color: Theme.textDim; font.pixelSize: 10; font.family: Theme.sans }
                        C.Popup {
                            id: zoomPop
                            y: parent.height + 4; x: parent.width - implicitWidth
                            padding: 4
                            background: Rectangle { color: Theme.panel2; border.color: Theme.line; border.width: 1; radius: 6 }
                            contentItem: Column {
                                spacing: 1
                                Repeater {
                                    model: [{ t: "Ajustar", z: 0 }, { t: "50%", z: 0.5 }, { t: "100%", z: 1.0 }, { t: "200%", z: 2.0 }]
                                    delegate: Rectangle {
                                        required property var modelData
                                        readonly property bool isCurrent: mon.viewerZoom === modelData.z
                                        width: 110; height: 24; radius: 4
                                        color: zoptHover.hovered ? Theme.hover : "transparent"
                                        HoverHandler { id: zoptHover }
                                        TapHandler { onTapped: { mon.viewerZoom = modelData.z; zoomPop.close() } }
                                        Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: modelData.t
                                               color: parent.isCurrent ? Theme.amber : Theme.textHi
                                               font.pixelSize: 11; font.family: Theme.sans
                                               font.weight: parent.isCurrent ? Font.DemiBold : Font.Normal }
                                    }
                                }
                            }
                        }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            }

            // Área de vídeo
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; color: "#000000"
                Rectangle {
                    id: screen
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 28, (parent.height - 28) * 16 / 9)
                    height: width * 9 / 16
                    color: screenBg; border.color: Theme.line; border.width: 1
                    clip: true

                    // Fondo negro para las bandas de letterbox
                    Rectangle { anchors.fill: parent; color: "#000000"; visible: showVideo }
                    // Vídeo compuesto por GPU (RHI): ORIGEN = VideoController, PROGRAMA = Compositor
                    VideoSurface {
                        anchors.fill: parent
                        visible: showVideo
                        source: program ? Compositor : ctrl
                        zoom: mon.viewerZoom
                        panX: mon.viewPanX
                        panY: mon.viewPanY
                    }
                    // Paneo del visor con zoom: arrastrar mueve el encuadre
                    // (doble clic = recentrar).
                    MouseArea {
                        visible: mon.viewerZoom > 0 && mon.showVideo
                        anchors.fill: parent
                        cursorShape: Qt.OpenHandCursor
                        property real bx; property real by; property real px0; property real py0
                        onPressed: (m) => { bx = mon.viewPanX; by = mon.viewPanY; px0 = m.x; py0 = m.y }
                        onPositionChanged: (m) => { mon.viewPanX = bx + (m.x - px0); mon.viewPanY = by + (m.y - py0) }
                        onDoubleClicked: { mon.viewPanX = 0; mon.viewPanY = 0 }
                    }

                    Text { visible: !showVideo; anchors.centerIn: parent; text: caption; color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.mono; font.letterSpacing: 1 }

                    // Manipulador on-screen (PROGRAMA): caja de transformación REAL del
                    // clip seleccionado. Arrastrar el cuerpo = posición (doble clic =
                    // centrar); arrastrar una esquina = escala desde el centro. Solo en
                    // modo Ajustar (con zoom del visor el mapeo no coincide). La caja
                    // aproxima el tamaño con lienzo×escala (exacto para medios 16:9).
                    Item {
                        id: manip
                        anchors.fill: parent
                        visible: mon.program && mon.progLive && TimelineModel.hasSelection
                                 && !TimelineModel.selIsAudio && mon.viewerZoom === 0
                        Rectangle {
                            id: manipBox
                            x: manip.width * (0.5 + TimelineModel.selPosX) - width / 2
                            y: manip.height * (0.5 + TimelineModel.selPosY) - height / 2
                            width: Math.max(24, manip.width * TimelineModel.selScale)
                            height: Math.max(14, manip.height * TimelineModel.selScale)
                            rotation: TimelineModel.selRotation
                            color: "transparent"; border.color: Theme.amber; border.width: 1.5
                            Rectangle { width: 9; height: 9; radius: 5; color: "transparent"; border.color: Theme.amber; border.width: 1.5; anchors.centerIn: parent }
                            // Cuerpo: posición
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.SizeAllCursor
                                property real bx; property real by; property var p0
                                onPressed: (m) => { bx = TimelineModel.selPosX; by = TimelineModel.selPosY; p0 = mapToItem(manip, m.x, m.y) }
                                onPositionChanged: (m) => {
                                    var p = mapToItem(manip, m.x, m.y)
                                    TimelineModel.setSelPosX(bx + (p.x - p0.x) / manip.width)
                                    TimelineModel.setSelPosY(by + (p.y - p0.y) / manip.height)
                                }
                                onDoubleClicked: { TimelineModel.setSelPosX(0); TimelineModel.setSelPosY(0) }
                            }
                            // Tirador de rotación: círculo sobre el borde superior;
                            // arrastrar gira la capa (doble clic = 0°).
                            Rectangle { width: 1.5; height: 14; color: Theme.amber; x: manipBox.width / 2 - 0.75; y: -14 }
                            Rectangle {
                                width: 11; height: 11; radius: 6
                                color: rotMA.pressed ? Theme.amber : "transparent"
                                border.color: Theme.amber; border.width: 1.5
                                x: manipBox.width / 2 - 5.5; y: -25
                                MouseArea {
                                    id: rotMA
                                    anchors.fill: parent; anchors.margins: -5
                                    cursorShape: Qt.CrossCursor
                                    function angle(mx, my) {
                                        var p = mapToItem(manip, mx, my)
                                        var cx = manip.width * (0.5 + TimelineModel.selPosX)
                                        var cy = manip.height * (0.5 + TimelineModel.selPosY)
                                        // El tirador está ARRIBA del centro: ese rayo es 0°.
                                        return Math.atan2(p.x - cx, -(p.y - cy)) * 180 / Math.PI
                                    }
                                    onPositionChanged: (m) => TimelineModel.setSelRotation(angle(m.x, m.y))
                                    onDoubleClicked: TimelineModel.setSelRotation(0)
                                }
                            }
                            // Esquinas: escala (distancia al centro)
                            Repeater {
                                model: [[0, 0], [1, 0], [0, 1], [1, 1]]
                                delegate: Rectangle {
                                    required property var modelData
                                    width: 8; height: 8; color: Theme.amber
                                    x: manipBox.width * modelData[0] - 4
                                    y: manipBox.height * modelData[1] - 4
                                    MouseArea {
                                        anchors.fill: parent; anchors.margins: -4
                                        cursorShape: Qt.SizeFDiagCursor
                                        property real s0; property real d0
                                        function dist(mx, my) {
                                            var p = mapToItem(manip, mx, my)
                                            var cx = manip.width * (0.5 + TimelineModel.selPosX)
                                            var cy = manip.height * (0.5 + TimelineModel.selPosY)
                                            return Math.max(8, Math.hypot(p.x - cx, p.y - cy))
                                        }
                                        onPressed: (m) => { s0 = TimelineModel.selScale; d0 = dist(m.x, m.y) }
                                        onPositionChanged: (m) => TimelineModel.setSelScale(
                                            Math.max(0.05, Math.min(4, s0 * dist(m.x, m.y) / d0)))
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Transporte
            Rectangle {
                Layout.fillWidth: true; height: 52; color: Theme.panel3
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
                ColumnLayout {
                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 5
                    Item { Layout.fillHeight: true }
                    // Scrubber
                    Rectangle {
                        id: scrub
                        Layout.fillWidth: true; height: 4; radius: 2; color: Theme.sunken
                        readonly property real frac: program ? TimelineModel.playheadFraction
                                                             : (live ? ctrl.fraction : fillW)
                        Rectangle { height: parent.height; radius: 2; color: "#4a4d55"; width: scrub.width * scrub.frac }
                        Rectangle { width: 2; height: 10; color: Theme.amber; x: scrub.width * scrub.frac - 1; y: -3 }
                        TapHandler {
                            enabled: live || program
                            onTapped: (p) => {
                                if (program) TimelineModel.setPlayheadFraction(p.position.x / scrub.width)
                                else ctrl.seekFraction(p.position.x / scrub.width)
                            }
                        }
                    }
                    // Timecodes + controles
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: program ? mon.seqTc(TimelineModel.playheadUs)
                                             : (live ? ctrl.positionTc : tcLeft)
                               color: tcLeftColor; font.pixelSize: 11; font.family: Theme.mono }
                        Item { Layout.fillWidth: true }
                        Row {
                            spacing: 14
                            Text { text: "◀◀"; color: (program || live) ? Theme.text : Theme.textFaint; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter
                                   TapHandler { onTapped: mon.stepFrames(-1) }
                                   Tip { edge: "top"; text: "Fotograma anterior" } }
                            Rectangle {
                                visible: program; width: 24; height: 24; radius: 12; color: Theme.amber; anchors.verticalCenter: parent.verticalCenter
                                Text { anchors.centerIn: parent; text: Compositor.playing ? "❚❚" : "▶"; color: Theme.amberInk; font.pixelSize: 12 }
                                TapHandler { onTapped: Compositor.togglePlay() }
                                Tip { edge: "top"; text: Compositor.playing ? "Pausar (Espacio)" : "Reproducir (Espacio)" }
                            }
                            Text { visible: !program; text: (live && ctrl.playing) ? "❚❚" : "▶"; color: Theme.text; font.pixelSize: 15; anchors.verticalCenter: parent.verticalCenter
                                   TapHandler { onTapped: if (live) ctrl.togglePlay() }
                                   Tip { edge: "top"; text: (live && ctrl.playing) ? "Pausar" : "Reproducir" } }
                            Text { text: "▶▶"; color: (program || live) ? Theme.text : Theme.textFaint; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter
                                   TapHandler { onTapped: mon.stepFrames(1) }
                                   Tip { edge: "top"; text: "Fotograma siguiente" } }
                            // ● = guardar el fotograma actual del PROGRAMA como PNG.
                            Text { visible: program; text: "●"; color: Theme.red; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter
                                   opacity: Compositor.hasContent ? 1.0 : 0.35
                                   TapHandler { enabled: Compositor.hasContent; onTapped: Compositor.saveStillDialog() }
                                   Tip { edge: "top"; text: "Guardar fotograma actual como PNG" } }
                        }
                        Item { Layout.fillWidth: true }
                        Text { text: program ? mon.seqTc(TimelineModel.contentEndUs)
                                             : (live ? ctrl.durationTc : tcRight)
                               color: Theme.textDim; font.pixelSize: 11; font.family: Theme.mono }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Monitor {
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "ORIGEN"; clipName: "—"
            screenBg: "#20272e"; caption: "SIN CLIP · doble clic en un medio"; program: false
            fillW: 0; tcLeft: "00:00:00:00"; tcRight: "00:00:00:00"
            ctrl: VideoController
        }
        Monitor {
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "PROGRAMA"; titleColor: Theme.amber; clipName: "Timeline"
            screenBg: "#2a2f26"; caption: "SIN CONTENIDO · añade clips a la línea de tiempo"; program: true
            fillW: 0; tcLeftColor: Theme.amber
        }
    }
}
