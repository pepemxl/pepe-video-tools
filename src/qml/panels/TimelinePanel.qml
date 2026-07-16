import QtQuick
import QtQuick.Layouts
import PepeVideo

// Región inferior: barra de herramientas + pistas + mezclador.
Rectangle {
    id: tlRoot
    height: 334
    color: Theme.bg2

    property int currentTool: 0   // A = Selección
    // El imán real vive en el modelo (afecta al arrastre/recorte).
    readonly property bool snap: TimelineModel.snapEnabled

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
                // Zoom
                Row {
                    spacing: 6; Layout.alignment: Qt.AlignVCenter
                    Text { text: "Zoom"; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.sans; anchors.verticalCenter: parent.verticalCenter }
                    Rectangle { width: 120; height: 5; radius: 3; color: Theme.sunken; anchors.verticalCenter: parent.verticalCenter
                        Rectangle { width: parent.width*0.46; height: parent.height; radius: 3; color: "#4a4d55" }
                        Rectangle { width: 11; height: 11; radius: 6; color: Theme.text; x: parent.width*0.46 - 5.5; y: -3 } }
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
                                   text: "00:04:12:08"; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.mono } }
                        Item {
                            id: scaleArea
                            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                            TapHandler { onTapped: (p) => TimelineModel.setPlayheadFraction(p.position.x / scaleArea.width) }
                            Repeater {
                                model: [ {p:0.06,t:"00:00"},{p:0.24,t:"01:00"},{p:0.42,t:"02:00"},{p:0.60,t:"03:00"},{p:0.78,t:"04:00"},{p:0.96,t:"05:00"} ]
                                delegate: Item {
                                    required property var modelData
                                    anchors.top: parent.top; anchors.bottom: parent.bottom
                                    x: parent.width * modelData.p
                                    Rectangle { width: 1; height: parent.height; color: Theme.line }
                                    Text { text: modelData.t; color: Theme.textFaint; font.pixelSize: 9; font.family: Theme.mono; x: 2; y: 5 }
                                }
                            }
                            // Marcadores (desde el modelo); clic para eliminar
                            Repeater {
                                model: TimelineModel.markers
                                delegate: Item {
                                    required property var modelData
                                    anchors.top: parent.top; anchors.bottom: parent.bottom
                                    x: scaleArea.width * modelData.x - 4.5
                                    width: 9
                                    Canvas { anchors.top: parent.top; width: 9; height: 11
                                        onPaint: { var c=getContext("2d"); c.fillStyle=modelData.color; c.beginPath()
                                            c.moveTo(0,0);c.lineTo(9,0);c.lineTo(9,6.6);c.lineTo(4.5,11);c.lineTo(0,6.6); c.closePath(); c.fill() } }
                                    Rectangle { x: 4; width: 1; height: parent.height; color: modelData.color; opacity: 0.35 }
                                    TapHandler { onTapped: TimelineModel.removeMarkerNear(modelData.x) }
                                }
                            }
                            // Playhead (desde el modelo)
                            Rectangle { width: 1; height: parent.height; color: Theme.amber; x: scaleArea.width * TimelineModel.playheadFraction }
                            Canvas { anchors.top: parent.top; width: 11; height: 9; x: scaleArea.width * TimelineModel.playheadFraction - 5.5
                                onPaint: { var c=getContext("2d"); c.fillStyle=Theme.amber; c.beginPath(); c.moveTo(0,0);c.lineTo(11,0);c.lineTo(5.5,9); c.closePath(); c.fill() } }
                        }
                    }
                }

                // Pistas
                Flickable {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
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
                                        Repeater { model: modelData.kind === "video" ? ["👁","🔒"] : ["M","S"]
                                            delegate: Rectangle { required property string modelData; width: 16; height: 14; radius: 3; color: Theme.hover2
                                                Text { anchors.centerIn: parent; text: modelData; font.pixelSize: 8; color: Theme.textDim } } } }
                                }
                                // Lane con clips
                                Item {
                                    id: lane
                                    width: parent.width - 158; height: parent.height
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#202127" }

                                    Repeater {
                                        model: modelData.clips
                                        delegate: Rectangle {
                                            id: clip
                                            required property var modelData
                                            readonly property bool isTitle: modelData.kind === "title"
                                            readonly property bool isAudio: modelData.kind === "audio"
                                            // Desplazamientos en vivo mientras se arrastra (px):
                                            property real mvDx: 0   // mover clip (horizontal)
                                            property real mvDy: 0   // mover clip (vertical, entre pistas)
                                            property real tlDx: 0   // recortar borde izquierdo
                                            property real trDx: 0   // recortar borde derecho
                                            readonly property bool moveTool: tlRoot.currentTool === 0
                                            readonly property bool trimTool: tlRoot.currentTool === 6
                                            readonly property bool rollTool: tlRoot.currentTool === 4
                                            readonly property bool rippleTool: tlRoot.currentTool === 3
                                            x: lane.width * modelData.x + 1 + mvDx + tlDx
                                            y: 5 + (moveDrag.active ? mvDy : 0)
                                            z: moveDrag.active ? 20 : 0
                                            width: Math.max(6, lane.width * modelData.w - 2 - tlDx + trDx)
                                            height: parent.height - 10
                                            radius: 3; color: modelData.fill
                                            border.color: modelData.selected ? Theme.amber : modelData.border
                                            border.width: modelData.selected ? 2 : 1
                                            clip: true

                                            // Aplica la edición de un borde según la herramienta activa.
                                            function commitEdge(leftEdge, dpx) {
                                                var d = dpx / lane.width
                                                if (clip.trimTool) TimelineModel.trimClip(clip.modelData.id, leftEdge, d)
                                                else if (clip.rollTool) TimelineModel.rollEdit(clip.modelData.id, leftEdge, d)
                                                else if (clip.rippleTool && !leftEdge) TimelineModel.rippleTrimRight(clip.modelData.id, d)
                                            }

                                            // Arrastrar para mover (herramienta Selección · A); vertical = cambiar de pista
                                            DragHandler {
                                                id: moveDrag
                                                target: null
                                                enabled: clip.moveTool
                                                onActiveChanged: {
                                                    if (active) { clip.mvDx = 0; clip.mvDy = 0; TimelineModel.selectClip(clip.modelData.id); return }
                                                    var frac = clip.modelData.x + clip.mvDx / lane.width
                                                    var track = tlRoot.trackIndexAtSceneY(centroid.scenePosition.y)
                                                    clip.mvDx = 0; clip.mvDy = 0
                                                    TimelineModel.moveClipToFraction(clip.modelData.id, track, frac)
                                                }
                                                onActiveTranslationChanged: {
                                                    if (active) { clip.mvDx = activeTranslation.x; clip.mvDy = activeTranslation.y }
                                                }
                                            }
                                            // Franja superior (vídeo)
                                            Rectangle { visible: !clip.isAudio && !clip.isTitle; width: parent.width; height: Math.min(24, parent.height*0.45); color: "#10ffffff" }
                                            // Forma de onda (audio)
                                            Canvas { visible: clip.isAudio; anchors.fill: parent; anchors.margins: 4
                                                onPaint: { var c=getContext("2d"); c.clearRect(0,0,width,height); c.fillStyle = clip.modelData.wav
                                                    var mid=height/2; c.beginPath(); c.moveTo(0,mid)
                                                    for (var i=0;i<=width;i+=6){ var a=(Math.sin(i*0.35)+Math.sin(i*0.13))*0.25*height; c.lineTo(i,mid-a) }
                                                    for (var j=width;j>=0;j-=6){ var b=(Math.sin(j*0.35)+Math.sin(j*0.13))*0.25*height; c.lineTo(j,mid+b) }
                                                    c.closePath(); c.fill() } }
                                            // Etiqueta
                                            Text { visible: clip.modelData.name !== ""; text: clip.modelData.name
                                                   anchors.left: parent.left; anchors.leftMargin: 6
                                                   y: clip.isTitle ? (parent.height-height)/2 : 4
                                                   color: clip.modelData.selected ? "#ffffff" : (clip.isTitle ? "#e0d8f0" : "#cfe0ec")
                                                   font.pixelSize: clip.isTitle ? 10 : 9; font.family: Theme.sans; elide: Text.ElideRight
                                                   width: parent.width - 12 }
                                            // Tirador de borde izquierdo (Trim · W / Roll · N)
                                            Rectangle {
                                                width: 7; height: parent.height; anchors.left: parent.left
                                                visible: clip.trimTool || clip.rollTool
                                                color: trimLeft.active ? Theme.amber : "#33ffffff"
                                                DragHandler {
                                                    id: trimLeft; target: null; enabled: clip.trimTool || clip.rollTool
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
                                                    enabled: clip.trimTool || clip.rollTool || clip.rippleTool
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
                                                    if (tlRoot.currentTool === 2) // cuchilla
                                                        TimelineModel.splitAtFraction(clip.modelData.id,
                                                            clip.modelData.x + (p.position.x / clip.width) * clip.modelData.w)
                                                    else
                                                        TimelineModel.selectClip(clip.modelData.id)
                                                }
                                            }
                                        }
                                    }
                                    // Indicador de transición (solape de clips = disolvencia)
                                    Repeater {
                                        model: {
                                            var cs = modelData.clips.slice().sort((a, b) => a.x - b.x)
                                            var r = []
                                            for (var i = 1; i < cs.length; i++) {
                                                var pe = cs[i-1].x + cs[i-1].w
                                                if (cs[i].x < pe - 1e-6)
                                                    r.push({ x: cs[i].x, w: Math.min(pe, cs[i].x + cs[i].w) - cs[i].x })
                                            }
                                            return r
                                        }
                                        delegate: Rectangle {
                                            required property var modelData
                                            x: lane.width * modelData.x; y: 5
                                            width: Math.max(2, lane.width * modelData.w); height: parent.height - 10
                                            color: "#33e2a24b"; border.color: Theme.amber; border.width: 1; radius: 2
                                            Text { anchors.centerIn: parent; text: "⤫"; color: Theme.amber; font.pixelSize: 13 }
                                        }
                                    }
                                    // Playhead sobre la pista
                                    Rectangle { width: 1; height: parent.height; color: "#e2a24bcc"
                                                x: lane.width * TimelineModel.playheadFraction }
                                }
                            }
                        }
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
                            text: (Audio.lufs <= -60 ? "−∞" : Audio.lufs.toFixed(1)) + " LUFS"
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
                                required property var modelData
                                // Nivel real del motor de audio: MAIN = pico del master; pistas = envolvente A1–A3.
                                readonly property real lvl: modelData.main
                                    ? Math.max(Audio.peakL, Audio.peakR)
                                    : (Audio.trackPeaks[modelData.trk] || 0)
                                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 5
                                Text { Layout.alignment: Qt.AlignHCenter; text: modelData.id; color: modelData.col; font.pixelSize: 9; font.weight: Font.Bold; font.family: Theme.mono }
                                Rectangle { Layout.alignment: Qt.AlignHCenter; width: 20; height: 20; radius: 10; color: Theme.hover2; border.color: Theme.line2; border.width: 1
                                    Rectangle { width: 1.5; height: 8; color: modelData.main ? Theme.amber : Theme.text; x: parent.width/2 - 0.75; y: 2 } }
                                // Medidor + fader
                                RowLayout {
                                    Layout.alignment: Qt.AlignHCenter; Layout.fillHeight: true; spacing: 3
                                    Rectangle { width: 16; Layout.fillHeight: true; radius: 3; color: Theme.sunken; clip: true
                                        Rectangle { width: parent.width; anchors.bottom: parent.bottom
                                            height: parent.height * Math.min(1, lvl)
                                            gradient: Gradient { GradientStop { position: 0.0; color: "#4a9e6b" } GradientStop { position: 0.6; color: "#4a9e6b" } GradientStop { position: 1.0; color: modelData.main ? "#c0392b" : "#e2a24b" } }
                                            Behavior on height { NumberAnimation { duration: 80 } } } }
                                    Rectangle { width: 10; Layout.fillHeight: true; radius: 5; color: Theme.sunken
                                        Rectangle { width: 20; height: 11; radius: 2; color: "#3a3d45"; border.color: modelData.main ? Theme.amber : "#55575f"; border.width: 1
                                            x: parent.width/2 - 10; y: parent.height * modelData.cap } }
                                }
                                Text { Layout.alignment: Qt.AlignHCenter
                                    text: lvl > 0.0001 ? (20*Math.log10(lvl)).toFixed(1) : "−∞"
                                    color: modelData.main ? Theme.amber : Theme.textMid; font.pixelSize: 8; font.family: Theme.mono }
                                Row { Layout.alignment: Qt.AlignHCenter; spacing: 3
                                    Repeater { model: ["M","S"]; delegate: Rectangle { required property string modelData; width: 15; height: 14; radius: 3; color: Theme.hover2
                                        Text { anchors.centerIn: parent; text: modelData; font.pixelSize: 8; color: Theme.textDim } } } }
                            }
                        }
                    }
                }
            }
        }
    }
}
