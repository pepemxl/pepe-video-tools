import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import PepeVideo

// Página «Entregar»: ajustes de render + preview del PROGRAMA + cola de trabajos.
// Sustituye a monitores+timeline cuando el workspace activo es Entregar (modo 5).
Rectangle {
    id: root
    color: Theme.bg2

    // Formatea µs como HH:MM:SS.
    function fmtTc(us) {
        var s = Math.max(0, Math.floor(us / 1e6))
        var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60
        var p = (n) => (n < 10 ? "0" : "") + n
        return p(h) + ":" + p(m) + ":" + p(sec)
    }

    // Presets de exportación (mismos que la StatusBar).
    readonly property var presets: [
        { n: "YouTube 1080p", w: 1920, h: 1080, fps: 30.0, mbps: 12 },
        { n: "YouTube 4K",    w: 3840, h: 2160, fps: 30.0, mbps: 40 },
        { n: "Vimeo 1080p",   w: 1920, h: 1080, fps: 30.0, mbps: 16 },
        { n: "Borrador 720p", w: 1280, h: 720,  fps: 30.0, mbps: 8 }
    ]

    // ---- Desplegable reutilizable (etiqueta actual + opciones) ----
    component Combo: Rectangle {
        id: combo
        property string current: ""
        property var options: []
        signal picked(int index)
        Layout.fillWidth: true; implicitHeight: 28; radius: 5
        color: cbHover.hovered ? Theme.hover : Theme.sunken
        border.color: cbPop.visible ? Theme.amber : Theme.line; border.width: 1
        HoverHandler { id: cbHover }
        TapHandler { onTapped: cbPop.open() }
        Text { anchors.left: parent.left; anchors.leftMargin: 9; anchors.verticalCenter: parent.verticalCenter
               text: combo.current; color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans }
        Text { anchors.right: parent.right; anchors.rightMargin: 9; anchors.verticalCenter: parent.verticalCenter
               text: "▾"; color: Theme.textFaint; font.pixelSize: 10 }
        C.Popup {
            id: cbPop
            y: parent.height + 3; width: combo.width
            padding: 4
            background: Rectangle { color: Theme.panel2; border.color: Theme.line; border.width: 1; radius: 6 }
            contentItem: Column {
                spacing: 1
                Repeater {
                    model: combo.options
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        readonly property bool isCur: modelData === combo.current
                        width: cbPop.width - 8; height: 24; radius: 4
                        color: optHover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: optHover }
                        TapHandler { onTapped: { combo.picked(index); cbPop.close() } }
                        Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: modelData
                               color: parent.isCur ? Theme.amber : Theme.textHi
                               font.pixelSize: 11; font.family: Theme.sans
                               font.weight: parent.isCur ? Font.DemiBold : Font.Normal }
                    }
                }
            }
        }
    }

    // ---- Botón sólido ----
    component SolidBtn: Rectangle {
        id: sb
        property string label
        property bool primary: false
        property bool enabledBtn: true
        signal clicked()
        implicitWidth: sbTxt.width + 26; implicitHeight: 30; radius: 5
        opacity: enabledBtn ? 1.0 : 0.4
        color: !enabledBtn ? Theme.sunken
               : primary ? (sbHover.hovered ? Qt.lighter(Theme.amber, 1.08) : Theme.amber)
                         : (sbHover.hovered ? Theme.hover : Theme.sunken)
        border.color: primary ? Theme.amber : Theme.line; border.width: primary ? 0 : 1
        HoverHandler { id: sbHover }
        TapHandler { enabled: sb.enabledBtn; onTapped: sb.clicked() }
        Text { id: sbTxt; anchors.centerIn: parent; text: sb.label
               color: sb.primary ? Theme.amberInk : Theme.textHi
               font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
    }

    // ---- Etiqueta de una fila de ajuste ----
    component FLabel: Text {
        color: Theme.textMid; font.pixelSize: 11; font.family: Theme.sans
        Layout.preferredWidth: 84; Layout.alignment: Qt.AlignVCenter
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line3 }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // Cabecera
        Rectangle {
            Layout.fillWidth: true; height: 34; color: Theme.panelHead
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            Text { anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                   text: "Entregar · Render"; color: Theme.textHi; font.pixelSize: 12; font.weight: Font.DemiBold; font.family: Theme.sans }
            Text { anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter
                   text: Export.status; color: Export.running || Export.queueRunning ? Theme.amber : Theme.textDim
                   font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideMiddle; Layout.maximumWidth: 360 }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0

            // ===== Izquierda: ajustes de render =====
            Rectangle {
                Layout.preferredWidth: 380; Layout.fillHeight: true; color: Theme.panel
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
                Flickable {
                    anchors.fill: parent; clip: true
                    contentWidth: width; contentHeight: form.height
                    ColumnLayout {
                        id: form
                        width: parent.width; spacing: 16
                        Item { implicitHeight: 4; Layout.fillWidth: true }

                        // -- Archivo de salida --
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; spacing: 9
                            Text { text: "Archivo de salida"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                FLabel { text: "Nombre" }
                                Rectangle {
                                    Layout.fillWidth: true; implicitHeight: 28; radius: 5; color: Theme.sunken
                                    border.color: nameIn.activeFocus ? Theme.amber : Theme.line; border.width: 1
                                    TextInput {
                                        id: nameIn; anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
                                        verticalAlignment: Text.AlignVCenter; clip: true; selectByMouse: true
                                        color: Theme.textHi; font.pixelSize: 11; font.family: Theme.sans
                                        text: Export.outName
                                        onEditingFinished: Export.outName = text
                                        Keys.onReturnPressed: { Export.outName = text; focus = false }
                                    }
                                    Text { visible: nameIn.text === ""; anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter
                                           text: "pepe_export"; color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.sans }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                FLabel { text: "Carpeta" }
                                Rectangle {
                                    Layout.fillWidth: true; implicitHeight: 28; radius: 5; color: Theme.sunken; border.color: Theme.line; border.width: 1
                                    Text { anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; verticalAlignment: Text.AlignVCenter
                                           text: Export.outDir !== "" ? Export.outDir : "— (elige una carpeta)"
                                           color: Export.outDir !== "" ? Theme.textMid : Theme.textFaint
                                           font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideLeft }
                                }
                                SolidBtn { label: "…"; implicitWidth: 34; onClicked: Export.chooseOutputFile() }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "→ " + (Export.outDir !== "" ? Export.outDir + "/" : "") + (Export.outName !== "" ? Export.outName : "pepe_export") + "." + Export.outExt
                                color: Theme.textFaint; font.pixelSize: 9; font.family: Theme.mono; elide: Text.ElideMiddle
                            }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; implicitHeight: 1; color: Theme.lineSoft }

                        // -- Vídeo --
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; spacing: 9
                            Text { text: "Vídeo"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                FLabel { text: "Formato" }
                                Combo {
                                    current: Export.formatLabel
                                    options: Export.availableFormats.map(f => f.label)
                                    onPicked: (i) => Export.format = Export.availableFormats[i].id
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                FLabel { text: "Resolución" }
                                Combo {
                                    current: Export.outWidth + "×" + Export.outHeight
                                    options: ["1280×720", "1920×1080", "2560×1440", "3840×2160"]
                                    onPicked: (i) => { const p = options[i].split("×"); Export.setResolution(parseInt(p[0]), parseInt(p[1])) }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                FLabel { text: "FPS" }
                                Combo {
                                    current: (Export.outFps % 1 !== 0 ? Export.outFps.toFixed(2) : Export.outFps.toFixed(0)) + " fps"
                                    options: ["23.98 fps", "24 fps", "25 fps", "29.97 fps", "30 fps", "60 fps"]
                                    onPicked: (i) => Export.outFps = parseFloat(options[i])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                // ProRes/DNxHR: calidad por perfil, el bitrate no aplica.
                                opacity: Export.formatUsesBitrate ? 1.0 : 0.4
                                enabled: Export.formatUsesBitrate
                                FLabel { text: "Bitrate" }
                                Combo {
                                    current: Export.formatUsesBitrate ? Export.videoMbps + " Mb/s" : "(por perfil)"
                                    options: ["8 Mb/s", "12 Mb/s", "16 Mb/s", "24 Mb/s", "40 Mb/s"]
                                    onPicked: (i) => Export.videoMbps = parseInt(options[i])
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                FLabel { text: "Preset" }
                                Combo {
                                    current: Export.presetName
                                    options: root.presets.map(p => p.n)
                                    onPicked: (i) => { const p = root.presets[i]; Export.applyPreset(p.n, p.w, p.h, p.fps, p.mbps) }
                                }
                            }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; implicitHeight: 1; color: Theme.lineSoft }

                        // -- Rango de exportación (marcas I/O de la línea de tiempo) --
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; spacing: 6
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "Rango"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                                Item { Layout.fillWidth: true }
                                SolidBtn { label: "Usar todo"; implicitHeight: 22; visible: TimelineModel.hasInOut
                                           onClicked: TimelineModel.clearInOut() }
                            }
                            Text {
                                Layout.fillWidth: true
                                readonly property real durS: (TimelineModel.exportEndUs - TimelineModel.exportStartUs) / 1e6
                                text: TimelineModel.hasInOut
                                      ? (root.fmtTc(TimelineModel.exportStartUs) + " – " + root.fmtTc(TimelineModel.exportEndUs) + "  ·  " + durS.toFixed(1) + " s")
                                      : ("Todo el contenido  ·  " + durS.toFixed(1) + " s")
                                color: TimelineModel.hasInOut ? Theme.amber : Theme.textDim
                                font.pixelSize: 11; font.family: Theme.mono
                            }
                            Text { text: "Marca entrada/salida en la línea de tiempo (teclas I y O)"
                                   color: Theme.textFaint; font.pixelSize: 9; font.family: Theme.sans }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; implicitHeight: 1; color: Theme.lineSoft }

                        // -- Acciones --
                        RowLayout {
                            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; spacing: 8
                            SolidBtn { label: "Añadir a la cola"; enabledBtn: !Export.running && !Export.queueRunning
                                       onClicked: Export.enqueueCurrent() }
                            Item { Layout.fillWidth: true }
                            SolidBtn { label: "Exportar ahora"; primary: true; enabledBtn: !Export.running && !Export.queueRunning
                                       onClicked: Export.exportNow() }
                        }
                        Item { implicitHeight: 8; Layout.fillWidth: true }
                    }
                }
            }

            // ===== Derecha: preview + cola =====
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0

                // Preview del PROGRAMA
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: Math.round(parent.height * 0.42); color: "#000000"
                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 32, (parent.height - 24) * 16 / 9)
                        height: width * 9 / 16
                        color: "#111318"; border.color: Theme.line; border.width: 1; clip: true
                        VideoSurface { anchors.fill: parent; visible: Compositor.hasContent; source: Compositor; zoom: 0 }
                        Text { visible: !Compositor.hasContent; anchors.centerIn: parent
                               text: "SIN CONTENIDO"; color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.mono; font.letterSpacing: 1 }
                    }
                }

                // Cabecera de la cola
                Rectangle {
                    Layout.fillWidth: true; height: 34; color: Theme.panel2
                    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
                    Text { anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                           text: "Cola de render · " + Export.queue.length; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    Row {
                        anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                        SolidBtn { label: "Vaciar"; implicitHeight: 24; anchors.verticalCenter: parent.verticalCenter
                                   enabledBtn: Export.queue.length > 0 && !Export.queueRunning; onClicked: Export.clearQueue() }
                        SolidBtn { label: Export.queueRunning ? "Renderizando…" : "Renderizar cola"; primary: true; implicitHeight: 24
                                   anchors.verticalCenter: parent.verticalCenter
                                   enabledBtn: Export.queue.length > 0 && !Export.queueRunning && !Export.running
                                   onClicked: Export.startQueue() }
                    }
                }

                // Lista de trabajos
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.bg1
                    Text {
                        visible: Export.queue.length === 0
                        anchors.centerIn: parent; width: parent.width - 40; horizontalAlignment: Text.AlignHCenter
                        text: "La cola está vacía · configura la salida y pulsa «Añadir a la cola»"
                        color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.sans; wrapMode: Text.WordWrap
                    }
                    ListView {
                        anchors.fill: parent; anchors.margins: 8; clip: true; spacing: 5
                        model: Export.queue
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: ListView.view.width; height: 46; radius: 6
                            color: Theme.panel2; border.color: modelData.status === 1 ? Theme.amber : Theme.line; border.width: 1
                            readonly property color stCol: [Theme.textFaint, Theme.amber, Theme.green, Theme.red][modelData.status]
                            readonly property string stTxt: ["Pendiente", "En curso", "Hecho", "Error"][modelData.status]
                            Rectangle { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter
                                        width: 9; height: 9; radius: 5; color: parent.stCol }
                            Column {
                                anchors.left: parent.left; anchors.leftMargin: 30; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                                Text { text: modelData.name + ".mp4"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                                Text { text: modelData.width + "×" + modelData.height + " · "
                                             + (modelData.fps % 1 !== 0 ? modelData.fps.toFixed(2) : modelData.fps.toFixed(0)) + " fps · "
                                             + modelData.mbps + " Mb/s · " + modelData.preset
                                       color: Theme.textDim; font.pixelSize: 9; font.family: Theme.mono }
                            }
                            Row {
                                anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 10
                                Rectangle {
                                    visible: modelData.status === 1
                                    width: 90; height: 5; radius: 3; color: Theme.sunken; anchors.verticalCenter: parent.verticalCenter
                                    Rectangle { height: parent.height; radius: 3; color: Theme.amber; width: parent.width * Export.progress }
                                }
                                Text { text: modelData.status === 1 ? Math.round(Export.progress * 100) + " %" : stTxt
                                       color: stCol; font.pixelSize: 10; font.family: Theme.mono; anchors.verticalCenter: parent.verticalCenter }
                                Rectangle {
                                    visible: modelData.status !== 1
                                    width: 18; height: 18; radius: 4; anchors.verticalCenter: parent.verticalCenter
                                    color: rmHover.hovered ? Theme.red : Theme.hover2
                                    HoverHandler { id: rmHover }
                                    TapHandler { onTapped: Export.removeFromQueue(index) }
                                    Text { anchors.centerIn: parent; text: "✕"; font.pixelSize: 9; color: rmHover.hovered ? "#ffffff" : Theme.textDim }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
