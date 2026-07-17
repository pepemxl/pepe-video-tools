import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as C
import PepeVideo

// Región inferior: barra de herramientas + pistas + mezclador.
Rectangle {
    id: tlRoot
    height: 334
    color: Theme.bg2

    property int currentTool: 0   // A = Selección
    property real zoom: 1.0       // 1 = ajustar a la vista; >1 = ampliar horizontalmente
    property real scrollX: 0.0    // desplazamiento horizontal [0..1] cuando hay zoom
    // El imán real vive en el modelo (afecta al arrastre/recorte).
    readonly property bool snap: TimelineModel.snapEnabled

    // Formatea milisegundos como mm:ss (regla de tiempo).
    function fmtClock(ms) {
        var s = Math.max(0, Math.floor(ms / 1000))
        var m = Math.floor(s / 60); s = s % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    // Mapea una coordenada Y de escena al índice de pista (para arrastre vertical).
    function trackIndexAtSceneY(sceneY) {
        var y = tracksCol.mapFromItem(null, 0, sceneY).y
        var tr = TimelineModel.tracks
        var acc = 0
        for (var i = 0; i < tr.length; i++) {
            acc += tr[i].height
            if (y < acc) return i
        }
        return tr.length - 1
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line3 }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ===== Toolbar =====
        Rectangle {
            Layout.fillWidth: true; height: 40; color: Theme.panel2
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8

                // Herramientas
                Row {
                    spacing: 3
                    Repeater {
                        model: [
                            { g: "select", tip: "Selección (A)", sep: false },
                            { g: "track",  tip: "Selección de pista (T)", sep: true },
                            { g: "blade",  tip: "Cuchilla · Corte (B)", sep: false },
                            { g: "ripple", tip: "Ripple (RR)", sep: false },
                            { g: "roll",   tip: "Roll (N)", sep: false },
                            { g: "slip",   tip: "Deslizar · Slip (Y)", sep: false },
                            { g: "uslide", tip: "Slide (U)", sep: false },
                            { g: "trim",   tip: "Recorte fino · Trim (W)", sep: true },
                            { g: "pen",    tip: "Pluma · Keyframes (P)", sep: false },
                            { g: "zoom",   tip: "Zoom (Z)", sep: false }
                        ]
                        delegate: Row {
                            required property var modelData
                            required property int index
                            spacing: 3
                            Rectangle { visible: modelData.sep; width: 1; height: 18; color: Theme.line; anchors.verticalCenter: parent.verticalCenter }
                            Rectangle {
                                readonly property bool active: currentTool === index
                                width: 30; height: 28; radius: 5
                                color: active ? Theme.amber : (toolHover.hovered ? Theme.hover : "transparent")
                                anchors.verticalCenter: parent.verticalCenter
                                HoverHandler { id: toolHover }
                                TapHandler { onTapped: currentTool = index }
                                // Marca de la herramienta (letra corta)
                                Text { anchors.centerIn: parent; text: modelData.g.charAt(0).toUpperCase()
                                       color: parent.active ? Theme.amberInk : Theme.textMid
                                       font.pixelSize: 12; font.weight: Font.DemiBold; font.family: Theme.sans }
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Añadir título (Ctrl+T)
                Rectangle {
                    width: ttRow.width + 18; height: 28; radius: 5
                    color: ttHover.hovered ? Theme.hover : "transparent"; border.color: Theme.line; border.width: 1
                    HoverHandler { id: ttHover }
                    TapHandler { onTapped: TimelineModel.addTitleAtPlayhead() }
                    Row { id: ttRow; anchors.centerIn: parent; spacing: 5
                        Text { text: "T"; font.pixelSize: 12; font.weight: Font.Bold; font.family: Theme.sans
                               color: Theme.textHi; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: "+ Título"; font.pixelSize: 11; font.family: Theme.sans
                               color: Theme.textMid; anchors.verticalCenter: parent.verticalCenter } }
                }

                // Subtítulos: toggle CC + contador
                Rectangle {
                    width: srtRow.width + 16; height: 28; radius: 5; border.color: Theme.line; border.width: 1
                    color: TimelineModel.subtitlesEnabled ? Theme.blue : Theme.hover
                    TapHandler { onTapped: TimelineModel.subtitlesEnabled = !TimelineModel.subtitlesEnabled }
                    Row { id: srtRow; anchors.centerIn: parent; spacing: 5
                        Text { text: "CC"; font.pixelSize: 11; font.weight: Font.Bold; font.family: Theme.sans
                               color: TimelineModel.subtitlesEnabled ? "#0c1420" : Theme.textMid; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: TimelineModel.subtitleCount; font.pixelSize: 10; font.family: Theme.mono
                               color: TimelineModel.subtitlesEnabled ? "#0c1420" : Theme.textDim; anchors.verticalCenter: parent.verticalCenter } }
                }
                // Importar SRT
                Rectangle { width: 26; height: 28; radius: 5; border.color: Theme.line; border.width: 1
                    color: impHover.hovered ? Theme.hover : "transparent"
                    HoverHandler { id: impHover }
                    TapHandler { onTapped: TimelineModel.openImportSrtDialog() }
                    Text { anchors.centerIn: parent; text: "↓"; font.pixelSize: 14; color: Theme.textMid } }
                // Exportar SRT
                Rectangle { width: 26; height: 28; radius: 5; border.color: Theme.line; border.width: 1
                    color: expHover.hovered ? Theme.hover : "transparent"
                    HoverHandler { id: expHover }
                    TapHandler { onTapped: TimelineModel.openExportSrtDialog() }
                    Text { anchors.centerIn: parent; text: "↑"; font.pixelSize: 14; color: Theme.textMid } }

                // Marcador (añade en el playhead)
                Rectangle {
                    width: 28; height: 28; radius: 5; color: mkHover.hovered ? Theme.hover : "transparent"
                    HoverHandler { id: mkHover }
                    TapHandler { onTapped: TimelineModel.addMarkerAtPlayhead() }
                    Canvas { anchors.centerIn: parent; width: 9; height: 11
                        onPaint: { var c=getContext("2d"); c.fillStyle=Theme.amber; c.beginPath()
                            c.moveTo(0,0); c.lineTo(9,0); c.lineTo(9,7.7); c.lineTo(4.5,11); c.lineTo(0,7.7); c.closePath(); c.fill() } }
                }
                // Snap
                Rectangle {
                    width: snapRow.width + 20; height: 28; radius: 5
                    color: snap ? Theme.blue : Theme.hover
                    TapHandler { onTapped: TimelineModel.snapEnabled = !TimelineModel.snapEnabled }
                    Row { id: snapRow; anchors.centerIn: parent; spacing: 6
                        Text { text: "🧲"; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: "Imán"; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans
                               color: snap ? "#0c1420" : Theme.textMid; anchors.verticalCenter: parent.verticalCenter } }
                }
                // Zoom (arrastra; doble clic = ajustar a la vista)
                Row {
                    spacing: 6; Layout.alignment: Qt.AlignVCenter
                    Text { text: "Zoom"; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                    Rectangle { id: zoomTrack; width: 120; height: 5; radius: 3; color: Theme.sunken; anchors.verticalCenter: parent.verticalCenter
                        readonly property real maxZoom: 10
                        readonly property real f: (tlRoot.zoom - 1) / (maxZoom - 1)
                        Rectangle { width: zoomTrack.width * zoomTrack.f; height: parent.height; radius: 3; color: "#4a4d55" }
                        Rectangle { width: 11; height: 11; radius: 6; color: zoomDrag.pressed ? Theme.amber : Theme.text; x: zoomTrack.width * zoomTrack.f - 5.5; y: -3 }
                        MouseArea { id: zoomDrag; anchors.fill: parent; anchors.margins: -6
                            function upd(mx) { var f = Math.max(0, Math.min(1, mx / zoomTrack.width)); tlRoot.zoom = 1 + f * (zoomTrack.maxZoom - 1) }
                            onPressed: (m) => upd(m.x)
                            onPositionChanged: (m) => upd(m.x)
                            onDoubleClicked: tlRoot.zoom = 1 }
                    }
                    Text { text: tlRoot.zoom.toFixed(1) + "×"; color: Theme.textFaint; font.pixelSize: 9; font.family: Theme.mono; anchors.verticalCenter: parent.verticalCenter }
                }
            }
        }

        // ===== Cuerpo: pistas + mixer =====
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0

            // ----- Área de pistas -----
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0

                // Regla de tiempo
                Rectangle {
                    Layout.fillWidth: true; height: 26; color: Theme.panel3
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
                    RowLayout {
                        anchors.fill: parent; spacing: 0
                        Rectangle { Layout.preferredWidth: 158; Layout.fillHeight: true; color: "transparent"
                            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
                            Text { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter
                                   text: tlRoot.fmtClock(TimelineModel.playheadUs / 1000); color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono } }
                        Item {
                            id: scaleArea
                            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                            // Ancho de contenido (con zoom) y desplazamiento horizontal en px.
                            readonly property real contentW: width * tlRoot.zoom
                            readonly property real offX: -(contentW - width) * tlRoot.scrollX
                            TapHandler { onTapped: (p) => TimelineModel.setPlayheadFraction((p.position.x - scaleArea.offX) / scaleArea.contentW) }
                            // Marcas de tiempo reales (una cada ~minuto de la ventana), formateadas mm:ss.
                            Repeater {
                                model: 6
                                delegate: Item {
                                    required property int index
                                    readonly property real frac: index / 6
                                    anchors.top: parent.top; anchors.bottom: parent.bottom
                                    x: scaleArea.offX + scaleArea.contentW * frac
                                    Rectangle { width: 1; height: parent.height; color: Theme.line }
                                    Text { text: tlRoot.fmtClock(frac * TimelineModel.totalUsMs)
                                           color: Theme.textFaint; font.pixelSize: 9; font.family: Theme.mono; x: 2; y: 5 }
                                }
                            }
                            // Marcadores (desde el modelo); clic para eliminar
                            Repeater {
                                model: TimelineModel.markers
                                delegate: Item {
                                    required property var modelData
                                    anchors.top: parent.top; anchors.bottom: parent.bottom
                                    x: scaleArea.offX + scaleArea.contentW * modelData.x - 4.5
                                    width: 9
                                    Canvas { anchors.top: parent.top; width: 9; height: 11
                                        onPaint: { var c=getContext("2d"); c.fillStyle=modelData.color; c.beginPath()
                                            c.moveTo(0,0);c.lineTo(9,0);c.lineTo(9,6.6);c.lineTo(4.5,11);c.lineTo(0,6.6); c.closePath(); c.fill() } }
                                    Rectangle { x: 4; width: 1; height: parent.height; color: modelData.color; opacity: 0.35 }
                                    TapHandler { onTapped: TimelineModel.removeMarkerNear(modelData.x) }
                                }
                            }
                            // Playhead (desde el modelo)
                            Rectangle { width: 1; height: parent.height; color: Theme.amber; x: scaleArea.offX + scaleArea.contentW * TimelineModel.playheadFraction }
                            Canvas { anchors.top: parent.top; width: 11; height: 9; x: scaleArea.offX + scaleArea.contentW * TimelineModel.playheadFraction - 5.5
                                onPaint: { var c=getContext("2d"); c.fillStyle=Theme.amber; c.beginPath(); c.moveTo(0,0);c.lineTo(11,0);c.lineTo(5.5,9); c.closePath(); c.fill() } }
                        }
                    }
                }

                // Pistas (+ zona de soltado para arrastrar medios desde el Media Pool)
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                Flickable {
                    id: tracksFlick
                    anchors.fill: parent; clip: true
                    contentWidth: width; contentHeight: tracksCol.height
                    Column {
                        id: tracksCol; width: parent.width
                        // Pistas y clips desde TimelineModel (C++)
                        Repeater {
                            model: TimelineModel.tracks
                            delegate: Row {
                                id: trackRow
                                required property var modelData
                                required property int index
                                readonly property int trackIndex: index
                                width: tracksCol.width; height: modelData.height
                                // Cabecera de pista
                                Rectangle {
                                    width: 158; height: parent.height; color: Theme.panel2
                                    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#202127" }
                                    Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter
                                           text: modelData.name; color: modelData.idColor; font.pixelSize: 10; font.weight: Font.Bold; font.family: Theme.mono }
                                    Row { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                                        Repeater { model: trackRow.modelData.kind === "video" ? ["👁","🔒"] : ["M","S"]
                                            delegate: Rectangle {
                                                id: hdrBtn
                                                required property string modelData
                                                required property int index
                                                readonly property bool isVideo: trackRow.modelData.kind === "video"
                                                readonly property bool first: index === 0   // 👁 / M
                                                readonly property bool on: isVideo
                                                    ? (first ? trackRow.modelData.hidden : trackRow.modelData.locked)
                                                    : (first ? trackRow.modelData.mute : trackRow.modelData.solo)
                                                width: 16; height: 14; radius: 3
                                                color: on ? (isVideo ? Theme.amber : (first ? Theme.amber : Theme.blue)) : Theme.hover2
                                                Text { anchors.centerIn: parent; text: hdrBtn.modelData; font.pixelSize: 8
                                                       color: hdrBtn.on ? "#101216" : Theme.textDim }
                                                TapHandler {
                                                    onTapped: {
                                                        var ti = trackRow.trackIndex
                                                        if (hdrBtn.isVideo) {
                                                            if (hdrBtn.first) TimelineModel.setTrackHidden(ti, !trackRow.modelData.hidden)
                                                            else TimelineModel.setTrackLocked(ti, !trackRow.modelData.locked)
                                                        } else {
                                                            if (hdrBtn.first) TimelineModel.setTrackMute(ti, !trackRow.modelData.mute)
                                                            else TimelineModel.setTrackSolo(ti, !trackRow.modelData.solo)
                                                        }
                                                    }
                                                }
                                            } } }
                                }
                                // Lane con clips
                                Item {
                                    id: lane
                                    width: parent.width - 158; height: parent.height
                                    clip: true
                                    // Ancho de contenido (con zoom) y desplazamiento horizontal en px,
                                    // compartidos con la regla para que todo se mueva en sincronía.
                                    readonly property real contentW: width * tlRoot.zoom
                                    readonly property real offX: -(contentW - width) * tlRoot.scrollX
                                    property real trackShiftDx: 0   // desplazamiento en vivo de toda la pista (herramienta T)
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#202127" }

                                    Repeater {
                                        model: modelData.clips
                                        delegate: Rectangle {
                                            id: clip
                                            required property var modelData
                                            readonly property bool isTitle: modelData.kind === "title"
                                            readonly property bool isAudio: modelData.kind === "audio"
                                            readonly property bool locked: trackRow.modelData.locked === true
                                            // Desplazamientos en vivo mientras se arrastra (px):
                                            property real mvDx: 0   // mover clip (horizontal)
                                            property real mvDy: 0   // mover clip (vertical, entre pistas)
                                            property real tlDx: 0   // recortar borde izquierdo
                                            property real trDx: 0   // recortar borde derecho
                                            property real slipDx: 0 // deslizar contenido (no mueve el clip)
                                            readonly property bool moveTool: tlRoot.currentTool === 0
                                            readonly property bool trimTool: tlRoot.currentTool === 7
                                            readonly property bool rollTool: tlRoot.currentTool === 4
                                            readonly property bool rippleTool: tlRoot.currentTool === 3
                                            readonly property bool slipTool: tlRoot.currentTool === 5
                                            readonly property bool slideTool: tlRoot.currentTool === 6
                                            readonly property bool trackTool: tlRoot.currentTool === 1
                                            readonly property bool penTool: tlRoot.currentTool === 8
                                            x: lane.offX + lane.contentW * modelData.x + 1 + mvDx + tlDx + lane.trackShiftDx
                                            y: 5 + (moveDrag.active ? mvDy : 0)
                                            z: moveDrag.active ? 20 : 0
                                            width: Math.max(6, lane.contentW * modelData.w - 2 - tlDx + trDx)
                                            height: parent.height - 10
                                            radius: 3; color: modelData.fill
                                            border.color: modelData.selected ? Theme.amber : modelData.border
                                            border.width: modelData.selected ? 2 : 1
                                            clip: true

                                            // Aplica la edición de un borde según la herramienta activa.
                                            function commitEdge(leftEdge, dpx) {
                                                var d = dpx / lane.contentW
                                                if (clip.trimTool) TimelineModel.trimClip(clip.modelData.id, leftEdge, d)
                                                else if (clip.rollTool) TimelineModel.rollEdit(clip.modelData.id, leftEdge, d)
                                                else if (clip.rippleTool) leftEdge ? TimelineModel.rippleTrimLeft(clip.modelData.id, d)
                                                                                   : TimelineModel.rippleTrimRight(clip.modelData.id, d)
                                            }

                                            // Arrastrar para mover (herramienta Selección · A); vertical = cambiar de pista.
                                            // Con Slip (Y) el mismo arrastre desliza el contenido (in-point); con
                                            // Slide (U) desplaza el clip entre sus vecinos adyacentes.
                                            DragHandler {
                                                id: moveDrag
                                                target: null
                                                enabled: (clip.moveTool || clip.slipTool || clip.slideTool || clip.trackTool) && !clip.locked
                                                onActiveChanged: {
                                                    if (active) { clip.mvDx = 0; clip.mvDy = 0; clip.slipDx = 0; lane.trackShiftDx = 0; TimelineModel.selectClip(clip.modelData.id); return }
                                                    if (clip.slipTool) {
                                                        TimelineModel.slipClip(clip.modelData.id, clip.slipDx / lane.contentW)
                                                        clip.slipDx = 0
                                                        return
                                                    }
                                                    if (clip.slideTool) {
                                                        var sd = clip.mvDx / lane.contentW
                                                        clip.mvDx = 0
                                                        TimelineModel.slideClip(clip.modelData.id, sd)
                                                        return
                                                    }
                                                    if (clip.trackTool) {
                                                        TimelineModel.shiftTrack(trackRow.trackIndex, lane.trackShiftDx / lane.contentW)
                                                        lane.trackShiftDx = 0
                                                        return
                                                    }
                                                    var frac = clip.modelData.x + clip.mvDx / lane.contentW
                                                    var track = tlRoot.trackIndexAtSceneY(centroid.scenePosition.y)
                                                    clip.mvDx = 0; clip.mvDy = 0
                                                    TimelineModel.moveClipToFraction(clip.modelData.id, track, frac)
                                                }
                                                onActiveTranslationChanged: {
                                                    // Slip y Track no mueven el clip individual: registran su propio desplazamiento.
                                                    if (!active) return
                                                    if (clip.slipTool) clip.slipDx = activeTranslation.x
                                                    else if (clip.trackTool) lane.trackShiftDx = activeTranslation.x
                                                    else if (clip.slideTool) clip.mvDx = activeTranslation.x
                                                    else { clip.mvDx = activeTranslation.x; clip.mvDy = activeTranslation.y }
                                                }
                                            }
                                            // Franja superior (vídeo)
                                            Rectangle { visible: !clip.isAudio && !clip.isTitle; width: parent.width; height: Math.min(24, parent.height*0.45); color: "#10ffffff" }
                                            // Forma de onda (audio): envolvente de PCM real si el clip tiene media
                                            // (Waveforms decodifica en un hilo y cachea); si no, onda sintética.
                                            Canvas { id: wavCanvas; visible: clip.isAudio; anchors.fill: parent; anchors.margins: 4
                                                readonly property string mediaPath: clip.modelData.mediaPath || ""
                                                onPaint: { var c=getContext("2d"); c.clearRect(0,0,width,height); c.fillStyle = clip.modelData.wav
                                                    var mid=height/2
                                                    var pk = mediaPath !== "" ? Waveforms.peaks(mediaPath, clip.modelData.inSec, clip.modelData.durSec,
                                                                                                clip.modelData.speed, Math.max(1, Math.floor(width/2))) : []
                                                    c.beginPath(); c.moveTo(0,mid)
                                                    if (pk.length > 0) {
                                                        var step = width/pk.length
                                                        for (var i=0;i<pk.length;++i) c.lineTo((i+0.5)*step, mid - Math.max(0.02, pk[i])*mid)
                                                        c.lineTo(width, mid)
                                                        for (var j=pk.length-1;j>=0;--j) c.lineTo((j+0.5)*step, mid + Math.max(0.02, pk[j])*mid)
                                                    } else {
                                                        for (var i2=0;i2<=width;i2+=6){ var a=(Math.sin(i2*0.35)+Math.sin(i2*0.13))*0.25*height; c.lineTo(i2,mid-a) }
                                                        for (var j2=width;j2>=0;j2-=6){ var b=(Math.sin(j2*0.35)+Math.sin(j2*0.13))*0.25*height; c.lineTo(j2,mid+b) }
                                                    }
                                                    c.closePath(); c.fill() }
                                                // Repinta cuando el worker termina de decodificar este archivo.
                                                Connections { target: Waveforms
                                                    function onReady(path) { if (path === wavCanvas.mediaPath) wavCanvas.requestPaint() } } }
                                            // Etiqueta
                                            Text { visible: clip.modelData.name !== ""; text: clip.modelData.name
                                                   anchors.left: parent.left; anchors.leftMargin: 6
                                                   y: clip.isTitle ? (parent.height-height)/2 : 4
                                                   color: clip.modelData.selected ? "#ffffff" : (clip.isTitle ? "#e0d8f0" : "#cfe0ec")
                                                   font.pixelSize: clip.isTitle ? 10 : 9; font.family: Theme.sans; elide: Text.ElideRight
                                                   width: parent.width - 12 }
                                            // Tirador de borde izquierdo (Trim · W / Roll · N / Ripple · RR)
                                            Rectangle {
                                                width: 7; height: parent.height; anchors.left: parent.left
                                                visible: clip.trimTool || clip.rollTool || clip.rippleTool
                                                color: trimLeft.active ? Theme.amber : "#33ffffff"
                                                DragHandler {
                                                    id: trimLeft; target: null
                                                    enabled: (clip.trimTool || clip.rollTool || clip.rippleTool) && !clip.locked
                                                    onActiveChanged: {
                                                        if (active) { clip.tlDx = 0; TimelineModel.selectClip(clip.modelData.id); return }
                                                        var dpx = clip.tlDx
                                                        clip.tlDx = 0
                                                        clip.commitEdge(true, dpx)
                                                    }
                                                    onActiveTranslationChanged: if (active) clip.tlDx = activeTranslation.x
                                                }
                                            }
                                            // Tirador de borde derecho (Trim · W / Roll · N / Ripple · RR)
                                            Rectangle {
                                                width: 7; height: parent.height; anchors.right: parent.right
                                                visible: clip.trimTool || clip.rollTool || clip.rippleTool
                                                color: trimRight.active ? Theme.amber : "#33ffffff"
                                                DragHandler {
                                                    id: trimRight; target: null
                                                    enabled: (clip.trimTool || clip.rollTool || clip.rippleTool) && !clip.locked
                                                    onActiveChanged: {
                                                        if (active) { clip.trDx = 0; TimelineModel.selectClip(clip.modelData.id); return }
                                                        var dpx = clip.trDx
                                                        clip.trDx = 0
                                                        clip.commitEdge(false, dpx)
                                                    }
                                                    onActiveTranslationChanged: if (active) clip.trDx = activeTranslation.x
                                                }
                                            }

                                            // Selección / cuchilla
                                            TapHandler {
                                                onTapped: (p) => {
                                                    var frac = clip.modelData.x + (p.position.x / clip.width) * clip.modelData.w
                                                    if (tlRoot.currentTool === 2 && !clip.locked) // cuchilla
                                                        TimelineModel.splitAtFraction(clip.modelData.id, frac)
                                                    else if (clip.penTool && !clip.locked)        // pluma: keyframe de opacidad
                                                        TimelineModel.penToggleKeyframe(clip.modelData.id, frac, "opacity")
                                                    else
                                                        TimelineModel.selectClip(clip.modelData.id)
                                                }
                                            }
                                        }
                                    }
                                    // Indicador de transición (solape de clips). Clic con la
                                    // herramienta Selección = editar tipo y duración.
                                    Repeater {
                                        model: {
                                            var cs = modelData.clips.slice().sort((a, b) => a.x - b.x)
                                            var r = []
                                            for (var i = 1; i < cs.length; i++) {
                                                var pe = cs[i-1].x + cs[i-1].w
                                                if (cs[i].x < pe - 1e-6)
                                                    r.push({ x: cs[i].x, w: Math.min(pe, cs[i].x + cs[i].w) - cs[i].x,
                                                             id: cs[i].id, type: cs[i].transition })
                                            }
                                            return r
                                        }
                                        delegate: Rectangle {
                                            id: ovl
                                            required property var modelData
                                            x: lane.offX + lane.contentW * modelData.x; y: 5
                                            width: Math.max(2, lane.contentW * modelData.w); height: parent.height - 10
                                            color: "#33e2a24b"; border.color: ovPop.visible ? "#ffffff" : Theme.amber; border.width: 1; radius: 2
                                            Text { anchors.centerIn: parent; text: "⤫"; color: Theme.amber; font.pixelSize: 13 }
                                            TapHandler { enabled: tlRoot.currentTool === 0; onTapped: ovPop.open() }
                                            C.Popup {
                                                id: ovPop
                                                y: parent.height + 2
                                                padding: 6
                                                background: Rectangle { color: Theme.panel2; border.color: Theme.line; border.width: 1; radius: 6 }
                                                contentItem: Column {
                                                    spacing: 3
                                                    Text { text: "Transición · " + (ovl.modelData.w * TimelineModel.totalUsMs / 1000).toFixed(2) + " s"
                                                           color: Theme.textHi; font.pixelSize: 10; font.family: Theme.sans; font.weight: Font.DemiBold }
                                                    Repeater {
                                                        model: [{ t: "Disolvencia", v: "cross" }, { t: "Fundido por negro", v: "dip" }, { t: "Barrido", v: "wipe" }]
                                                        delegate: Rectangle {
                                                            required property var modelData
                                                            readonly property bool isCurrent: ovl.modelData.type === modelData.v
                                                            width: 150; height: 22; radius: 4
                                                            color: ttypeHover.hovered ? Theme.hover : "transparent"
                                                            HoverHandler { id: ttypeHover }
                                                            TapHandler { onTapped: { TimelineModel.setClipTransition(ovl.modelData.id, modelData.v); ovPop.close() } }
                                                            Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: modelData.t
                                                                   color: parent.isCurrent ? Theme.amber : Theme.textHi
                                                                   font.pixelSize: 10; font.family: Theme.sans
                                                                   font.weight: parent.isCurrent ? Font.DemiBold : Font.Normal }
                                                        }
                                                    }
                                                    Rectangle { width: 150; height: 1; color: Theme.line }
                                                    Row {
                                                        spacing: 4
                                                        Text { text: "Duración"; color: Theme.textDim; font.pixelSize: 9; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                                                        Repeater {
                                                            model: [0.5, 1.0, 2.0]
                                                            delegate: Rectangle {
                                                                required property var modelData
                                                                width: 34; height: 20; radius: 4
                                                                color: durHover.hovered ? Theme.hover : Theme.sunken
                                                                border.color: Theme.line; border.width: 1
                                                                HoverHandler { id: durHover }
                                                                TapHandler { onTapped: { TimelineModel.setTransitionDuration(ovl.modelData.id, modelData); ovPop.close() } }
                                                                Text { anchors.centerIn: parent; text: modelData + " s"; color: Theme.textHi; font.pixelSize: 9; font.family: Theme.mono }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    // Playhead sobre la pista
                                    Rectangle { width: 1; height: parent.height; color: "#e2a24bcc"
                                                x: lane.offX + lane.contentW * TimelineModel.playheadFraction }
                                }
                            }
                        }
                    }
                }
                    // Zona de soltado: arrastrar un medio del Media Pool inserta un clip.
                    DropArea {
                        anchors.fill: parent
                        keys: ["application/x-pvs-media"]
                        onDropped: (drop) => {
                            // Los arrastres internos NO entregan mimeData: el payload viaja
                            // como propiedades del item de origen (drop.source, el fantasma).
                            var src = drop.source
                            if (!src || src.mediaName === undefined) return
                            var path = src.mediaPath
                            var kind = src.mediaKind
                            var nm = src.mediaName
                            var durStr = src.mediaDur
                            var us = 0
                            if (durStr && durStr.indexOf(":") >= 0) {
                                var pp = durStr.split(":"); us = (parseInt(pp[0]) * 60 + parseInt(pp[1])) * 1000000
                            }
                            var laneW = width - 158
                            var contentW = laneW * tlRoot.zoom
                            var offX = -(contentW - laneW) * tlRoot.scrollX
                            var fx = ((drop.x - 158) - offX) / contentW
                            var sceneY = mapToItem(null, drop.x, drop.y).y
                            var track = tlRoot.trackIndexAtSceneY(sceneY)
                            TimelineModel.addMediaClip(path, nm, kind, us, track, Math.max(0, Math.min(1, fx)))
                        }
                        // Realce del área de pistas mientras se arrastra encima.
                        Rectangle { anchors.fill: parent; anchors.leftMargin: 158
                            visible: parent.containsDrag; color: "#15e2a24b"; border.color: Theme.amber; border.width: 1 }
                    }
                    // Herramienta Zoom (Z): arrastrar en horizontal amplía/reduce el
                    // timeline (derecha = acercar); doble clic = ajustar a la vista.
                    MouseArea {
                        visible: tlRoot.currentTool === 9
                        anchors.fill: parent; anchors.leftMargin: 158
                        cursorShape: Qt.SizeHorCursor
                        preventStealing: true
                        property real baseZoom: 1
                        property real px: 0
                        onPressed: (m) => { baseZoom = tlRoot.zoom; px = m.x }
                        onPositionChanged: (m) => {
                            tlRoot.zoom = Math.max(1, Math.min(10, baseZoom * Math.exp((m.x - px) / 150)))
                        }
                        onDoubleClicked: tlRoot.zoom = 1
                    }
                }

                // Barra de desplazamiento horizontal (visible al ampliar con zoom)
                Rectangle {
                    Layout.fillWidth: true; height: 10; color: Theme.panel3; visible: tlRoot.zoom > 1.001
                    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }
                    Item {
                        id: hbarArea
                        anchors.left: parent.left; anchors.leftMargin: 158; anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter; height: parent.height
                        readonly property real barW: Math.max(24, width / tlRoot.zoom)
                        Rectangle { id: hbar; height: 6; radius: 3; anchors.verticalCenter: parent.verticalCenter
                            width: hbarArea.barW; color: hDrag.pressed ? Theme.amber : "#4a4d55"
                            x: (hbarArea.width - hbarArea.barW) * tlRoot.scrollX }
                        MouseArea { id: hDrag; anchors.fill: parent; cursorShape: Qt.SizeHorCursor
                            function upd(mx) { var range = hbarArea.width - hbarArea.barW
                                if (range > 0) tlRoot.scrollX = Math.max(0, Math.min(1, (mx - hbarArea.barW / 2) / range)) }
                            onPressed: (m) => upd(m.x)
                            onPositionChanged: (m) => upd(m.x) }
                    }
                }
            }

            // ----- Mezclador -----
            Rectangle {
                Layout.preferredWidth: 268; Layout.fillHeight: true; color: Theme.panel2
                Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.line }
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle {
                        Layout.fillWidth: true; height: 26; color: Theme.panelHead
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
                        Text { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: "Mezclador"; color: Theme.textHi; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                        Text { anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
                            // En reproducción muestra el loudness momentáneo (corto plazo); en reposo, el integrado.
                            readonly property real lu: Audio.playing ? Audio.lufsShort : Audio.lufs
                            text: (lu <= -60 ? "−∞" : lu.toFixed(1)) + (Audio.playing ? " LUFS-M" : " LUFS-I")
                            color: Theme.textDim; font.pixelSize: 10; font.family: Theme.sans }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 8; spacing: 4
                        Repeater {
                            model: [
                                { id: "A1", col: Theme.green,  trk: 3, cap: 0.34, db: "-3.2",  main: false },
                                { id: "A2", col: Theme.purple, trk: 4, cap: 0.52, db: "-9.5",  main: false },
                                { id: "A3", col: Theme.green,  trk: 5, cap: 0.64, db: "-14.0", main: false },
                                { id: "MAIN", col: Theme.amber, trk: -1, cap: 0.28, db: "0.0", main: true }
                            ]
                            delegate: ColumnLayout {
                                id: chan
                                required property var modelData
                                // Nivel real del motor de audio: MAIN = pico del master; pistas = envolvente A1–A3.
                                readonly property real lvl: modelData.main
                                    ? Math.max(Audio.peakL, Audio.peakR)
                                    : (Audio.trackPeaks[modelData.trk] || 0)
                                // Pista de audio asociada (null para MAIN): ganancia/paneo del fader.
                                readonly property var atrack: modelData.trk >= 0 ? TimelineModel.audioTracks[modelData.trk] : null
                                readonly property real trackGain: atrack ? atrack.gain : 1.0
                                readonly property real trackPan: atrack ? atrack.pan : 0.0
                                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 5
                                Text { Layout.alignment: Qt.AlignHCenter; text: modelData.id; color: modelData.col; font.pixelSize: 9; font.weight: Font.Bold; font.family: Theme.mono }
                                // Perilla de paneo: arrástrala en horizontal (doble clic = centro).
                                Rectangle { Layout.alignment: Qt.AlignHCenter; width: 20; height: 20; radius: 10; color: Theme.hover2; border.color: panKnob.pressed ? Theme.amber : Theme.line2; border.width: 1
                                    Rectangle { width: 1.5; height: 8; color: modelData.main ? Theme.amber : Theme.text
                                        x: parent.width/2 - 0.75; y: 2; transformOrigin: Item.Bottom; rotation: chan.trackPan * 135 }
                                    MouseArea {
                                        id: panKnob; anchors.fill: parent; anchors.margins: -3
                                        enabled: modelData.trk >= 0; cursorShape: Qt.SizeHorCursor
                                        property real base; property real px
                                        onPressed: (m) => { base = chan.trackPan; px = m.x }
                                        onPositionChanged: (m) => TimelineModel.setTrackPan(modelData.trk, base + (m.x - px) * 0.02)
                                        onDoubleClicked: TimelineModel.setTrackPan(modelData.trk, 0)
                                    }
                                }
                                // Medidor + fader
                                RowLayout {
                                    Layout.alignment: Qt.AlignHCenter; Layout.fillHeight: true; spacing: 3
                                    Rectangle { width: 16; Layout.fillHeight: true; radius: 3; color: Theme.sunken; clip: true
                                        Rectangle { width: parent.width; anchors.bottom: parent.bottom
                                            height: parent.height * Math.min(1, lvl)
                                            gradient: Gradient { GradientStop { position: 0.0; color: "#4a9e6b" } GradientStop { position: 0.6; color: "#4a9e6b" } GradientStop { position: 1.0; color: modelData.main ? "#c0392b" : "#e2a24b" } }
                                            Behavior on height { NumberAnimation { duration: 80 } } } }
                                    // Fader: arrástralo en vertical (doble clic = 0 dB). Rango 0…+12 dB.
                                    Rectangle { id: faderTrack; width: 10; Layout.fillHeight: true; radius: 5; color: Theme.sunken
                                        readonly property real capY: (1 - Math.min(1, chan.trackGain / 4)) * (height - 11)
                                        Rectangle { width: 20; height: 11; radius: 2; color: faderDrag.pressed ? Theme.amber : "#3a3d45"
                                            border.color: modelData.main ? Theme.amber : "#55575f"; border.width: 1
                                            x: parent.width/2 - 10; y: modelData.main ? parent.height * modelData.cap : faderTrack.capY }
                                        MouseArea {
                                            id: faderDrag; anchors.fill: parent; anchors.margins: -6
                                            enabled: modelData.trk >= 0; cursorShape: Qt.SizeVerCursor
                                            function setFromY(my) { var f = 1 - Math.max(0, Math.min(1, my / faderTrack.height)); TimelineModel.setTrackGain(modelData.trk, f * 4) }
                                            onPressed: (m) => setFromY(m.y)
                                            onPositionChanged: (m) => setFromY(m.y)
                                            onDoubleClicked: TimelineModel.setTrackGain(modelData.trk, 1.0)
                                        } }
                                }
                                Text { Layout.alignment: Qt.AlignHCenter
                                    text: lvl > 0.0001 ? (20*Math.log10(lvl)).toFixed(1) : "−∞"
                                    color: modelData.main ? Theme.amber : Theme.textMid; font.pixelSize: 8; font.family: Theme.mono }
                                Row { Layout.alignment: Qt.AlignHCenter; spacing: 3; visible: chan.modelData.trk >= 0
                                    Repeater { model: ["M","S"]
                                        delegate: Rectangle {
                                            required property string modelData
                                            readonly property bool isMute: modelData === "M"
                                            readonly property var at: TimelineModel.audioTracks[chan.modelData.trk]
                                            readonly property bool on: at ? (isMute ? at.mute : at.solo) : false
                                            width: 15; height: 14; radius: 3
                                            color: on ? (isMute ? Theme.amber : Theme.blue) : Theme.hover2
                                            Text { anchors.centerIn: parent; text: parent.modelData; font.pixelSize: 8
                                                   color: parent.on ? "#101216" : Theme.textDim }
                                            TapHandler {
                                                onTapped: parent.isMute
                                                    ? TimelineModel.setTrackMute(chan.modelData.trk, !parent.at.mute)
                                                    : TimelineModel.setTrackSolo(chan.modelData.trk, !parent.at.solo)
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
    }
}
