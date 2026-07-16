import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import PepeVideo

// Barra de estado / exportación.
Rectangle {
    id: sbRoot
    height: 38
    color: Theme.bg3

    // Presets de exportación (nombre + resolución + fps + bitrate).
    readonly property var exportPresets: [
        { n: "YouTube 1080p", w: 1920, h: 1080, fps: 30.0, mbps: 12 },
        { n: "YouTube 4K",    w: 3840, h: 2160, fps: 30.0, mbps: 40 },
        { n: "Vimeo 1080p",   w: 1920, h: 1080, fps: 30.0, mbps: 16 },
        { n: "Borrador 720p", w: 1280, h: 720,  fps: 30.0, mbps: 8 }
    ]

    // Chip con desplegable (se abre hacia arriba). Sin opciones = solo informativo.
    component Chip: Rectangle {
        id: chip
        property string label
        property color labelColor: Theme.text
        property var options: []      // etiquetas del desplegable
        property string current: ""   // opción marcada como activa
        signal picked(int index)
        readonly property bool interactive: options.length > 0 && !Export.running
        height: 26; width: chipContent.width + 18; radius: 5
        color: chipHover.hovered && interactive ? Theme.hover : Theme.sunken
        border.color: chipPop.visible ? Theme.amber : Theme.line; border.width: 1
        HoverHandler { id: chipHover }
        TapHandler { enabled: chip.interactive; onTapped: chipPop.open() }
        Row {
            id: chipContent; anchors.centerIn: parent; spacing: 5
            Text { text: chip.label; color: chip.labelColor; font.pixelSize: 10; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
            Text { visible: chip.options.length > 0; text: "▾"; color: Theme.textFaint; font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
        }
        C.Popup {
            id: chipPop
            y: -implicitHeight - 6
            padding: 4
            background: Rectangle { color: Theme.panel2; border.color: Theme.line; border.width: 1; radius: 6 }
            contentItem: Column {
                spacing: 1
                Repeater {
                    model: chip.options
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        readonly property bool isCurrent: modelData === chip.current
                        width: 150; height: 24; radius: 4
                        color: optHover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: optHover }
                        TapHandler { onTapped: { chip.picked(index); chipPop.close() } }
                        Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: modelData
                               color: parent.isCurrent ? Theme.amber : Theme.textHi
                               font.pixelSize: 11; font.family: Theme.sans
                               font.weight: parent.isCurrent ? Font.DemiBold : Font.Normal }
                    }
                }
            }
        }
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        // Info del proyecto (nombre y secuencia reales del modelo de proyecto)
        Row {
            spacing: 16
            Text { text: Project.baseName + " · " + Project.seqWidth + "×" + Project.seqHeight
                   color: Theme.textMid; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: Project.seqFps.toFixed(2) + " fps"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono }
            Text { text: Project.seqColorSpace; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono }
            // Estado de guardado real: hora del último guardado (auto o manual) o aviso.
            Text {
                text: Project.lastSavedText !== ""
                          ? "● " + (Project.lastSaveAuto ? "Autoguardado " : "Guardado ") + Project.lastSavedText
                          : (Project.dirty ? "● Sin guardar" : "○ Sin cambios")
                color: Project.lastSavedText !== "" ? Theme.green
                                                    : (Project.dirty ? Theme.amber : Theme.textDim)
                font.pixelSize: 10; font.family: Theme.mono
            }
        }

        Item { Layout.fillWidth: true }

        // Estado de exportación (progreso o resultado del último render).
        Text {
            visible: Export.running || Export.status !== ""
            text: Export.running ? ("Exportando… " + Math.round(Export.progress * 100) + " %") : Export.status
            color: Export.running ? Theme.amber : Theme.textDim
            font.pixelSize: 10; font.family: Theme.mono; elide: Text.ElideMiddle; Layout.maximumWidth: 240
        }

        Text { text: "Exportar:"; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.sans }

        // Formato (fijo: único códec de salida disponible)
        Chip { label: "MP4 · H.264" }
        // Resolución
        Chip {
            label: Export.outWidth + "×" + Export.outHeight
            current: label
            options: ["1280×720", "1920×1080", "2560×1440", "3840×2160"]
            onPicked: (i) => { const p = options[i].split("×"); Export.setResolution(parseInt(p[0]), parseInt(p[1])) }
        }
        // Fotogramas por segundo
        Chip {
            label: (Export.outFps % 1 !== 0 ? Export.outFps.toFixed(2) : Export.outFps.toFixed(0)) + " fps"
            current: label
            options: ["23.98 fps", "24 fps", "25 fps", "29.97 fps", "30 fps", "60 fps"]
            onPicked: (i) => Export.outFps = parseFloat(options[i])
        }
        // Bitrate de vídeo
        Chip {
            label: Export.videoMbps + " Mb/s"
            current: label
            options: ["8 Mb/s", "12 Mb/s", "16 Mb/s", "24 Mb/s", "40 Mb/s"]
            onPicked: (i) => Export.videoMbps = parseInt(options[i])
        }
        // Preset (fija resolución + fps + bitrate de una vez)
        Chip {
            label: "Preset: " + Export.presetName
            labelColor: Theme.blue
            current: Export.presetName
            options: sbRoot.exportPresets.map(p => p.n)
            onPicked: (i) => { const p = sbRoot.exportPresets[i]; Export.applyPreset(p.n, p.w, p.h, p.fps, p.mbps) }
        }

        // Botón exportar (abre el diálogo nativo de guardado y renderiza en 2.º plano).
        Rectangle {
            id: exportBtn
            height: 26; width: exp.width + 32; radius: 5; clip: true
            color: Export.running ? Theme.sunken : (exportHover.hovered ? Qt.lighter(Theme.amber, 1.08) : Theme.amber)
            border.color: Theme.amber; border.width: Export.running ? 1 : 0
            // Relleno de progreso durante la exportación.
            Rectangle { visible: Export.running; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                width: parent.width * Export.progress; color: "#553a2a0e" }
            HoverHandler { id: exportHover }
            TapHandler { enabled: !Export.running; onTapped: Export.openExportDialog() }
            Row {
                id: exp; anchors.centerIn: parent; spacing: 7
                Canvas {
                    width: 8; height: 10; anchors.verticalCenter: parent.verticalCenter
                    onPaint: { var c = getContext("2d"); c.fillStyle = Export.running ? Theme.amber : Theme.amberInk;
                        c.beginPath(); c.moveTo(0,0); c.lineTo(8,5); c.lineTo(0,10); c.closePath(); c.fill() }
                }
                Text { text: Export.running ? "Renderizando…" : "Exportar"
                       color: Export.running ? Theme.amber : Theme.amberInk
                       font.pixelSize: 11; font.weight: Font.Bold; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
            }
        }
    }
}
