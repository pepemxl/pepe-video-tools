import QtQuick
import QtQuick.Layouts
import PepeVideo

// Página «Fusión»: grafo de nodos del pipeline de composición del clip seleccionado
// (Fuente → Transformar → Color → Salida). Los nodos Transformar y Color se pueden
// OMITIR (bypass real: el compositor salta la etapa, visible en el preview). Pinchar
// un nodo enfoca su pestaña del Inspector. Visible solo en el workspace Fusión (modo 2).
Rectangle {
    id: root
    color: Theme.bg2

    // Pide al contenedor (Main) que fije la pestaña del Inspector (0 Inspector · 2 Color).
    signal stageSelected(int tab)

    readonly property bool hasSel: TimelineModel.hasSelection
    // ¿La transformación tiene ajustes (≠ identidad)?  ¿El color está graduado?
    readonly property bool tfActive: hasSel && (Math.abs(TimelineModel.selPosX) > 1e-4
                                     || Math.abs(TimelineModel.selPosY) > 1e-4
                                     || Math.abs(TimelineModel.selScale - 1) > 1e-4
                                     || Math.abs(TimelineModel.selRotation) > 1e-3)
    readonly property bool colActive: hasSel && (Math.abs(TimelineModel.selTemp) > 1e-4
                                      || Math.abs(TimelineModel.selTint) > 1e-4
                                      || Math.abs(TimelineModel.selSat - 1) > 1e-4
                                      || Math.abs(TimelineModel.selLiftX) > 1e-4 || Math.abs(TimelineModel.selLiftY) > 1e-4
                                      || Math.abs(TimelineModel.selGammaX) > 1e-4 || Math.abs(TimelineModel.selGammaY) > 1e-4
                                      || Math.abs(TimelineModel.selGainX) > 1e-4 || Math.abs(TimelineModel.selGainY) > 1e-4)

    // ---- Nodo del grafo ----
    component Node: Rectangle {
        id: node
        property string title
        property string subtitle
        property color accent: Theme.blue
        property bool bypassable: false
        property bool bypassed: false
        property bool dimmed: false           // sin selección
        signal toggleBypass()
        signal activated()
        width: 132; height: 66; radius: 8
        color: Theme.panel2
        border.color: bypassed ? Theme.line : accent
        border.width: bypassed ? 1 : 2
        opacity: dimmed ? 0.4 : (bypassed ? 0.6 : 1.0)
        // Puntos de conexión (izq/der)
        Rectangle { visible: node.title !== "Fuente"; width: 7; height: 7; radius: 4; color: node.accent
                    anchors.left: parent.left; anchors.leftMargin: -3; anchors.verticalCenter: parent.verticalCenter }
        Rectangle { visible: node.title !== "Salida"; width: 7; height: 7; radius: 4; color: node.accent
                    anchors.right: parent.right; anchors.rightMargin: -3; anchors.verticalCenter: parent.verticalCenter }
        HoverHandler { id: nodeHover }
        TapHandler { onTapped: node.activated() }
        Rectangle { anchors.fill: parent; radius: 8; color: nodeHover.hovered ? "#0affffff" : "transparent" }

        Column {
            anchors.left: parent.left; anchors.leftMargin: 12; anchors.right: parent.right; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter; spacing: 3
            Row {
                spacing: 6; width: parent.width
                Rectangle { width: 8; height: 8; radius: 4; color: node.accent; anchors.verticalCenter: parent.verticalCenter
                            visible: !node.bypassed }
                Rectangle { width: 8; height: 8; radius: 4; color: "transparent"; border.color: Theme.textFaint; border.width: 1
                            anchors.verticalCenter: parent.verticalCenter; visible: node.bypassed }
                Text { text: node.title; color: Theme.textHi; font.pixelSize: 12; font.weight: Font.DemiBold; font.family: Theme.sans
                       anchors.verticalCenter: parent.verticalCenter }
            }
            Text { text: node.bypassed ? "OMITIDO" : node.subtitle
                   color: node.bypassed ? Theme.amber : Theme.textDim; font.pixelSize: 9; font.family: Theme.mono; elide: Text.ElideRight; width: parent.width }
        }
        // Botón de omitir (bypass): interruptor arriba a la derecha
        Rectangle {
            visible: node.bypassable && !node.dimmed
            anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 6
            width: 18; height: 18; radius: 9
            color: node.bypassed ? Theme.hover2 : Qt.rgba(0.28, 0.62, 0.42, 0.25)
            border.color: node.bypassed ? Theme.line2 : Theme.green; border.width: 1
            HoverHandler { id: pwHover }
            TapHandler { onTapped: node.toggleBypass() }
            // Icono de encendido (⏻)
            Text { anchors.centerIn: parent; text: "⏻"; font.pixelSize: 10
                   color: node.bypassed ? Theme.textDim : Theme.green }
        }
    }

    // ---- Conector entre nodos ----
    component Connector: Rectangle {
        property bool live: true
        width: 26; height: 2; radius: 1
        color: live ? Theme.line2 : Theme.line
        anchors.verticalCenter: parent.verticalCenter
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line3 }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // Cabecera
        Rectangle {
            Layout.fillWidth: true; height: 34; color: Theme.panelHead
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
            Text { anchors.left: parent.left; anchors.leftMargin: 14; anchors.verticalCenter: parent.verticalCenter
                   text: "Fusión · Nodos"; color: Theme.textHi; font.pixelSize: 12; font.weight: Font.DemiBold; font.family: Theme.sans }
            Text { anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter
                   text: root.hasSel ? TimelineModel.selectedName : "Sin selección"
                   color: Theme.textDim; font.pixelSize: 10; font.family: Theme.sans; elide: Text.ElideMiddle; Layout.maximumWidth: 260 }
        }

        // Preview del PROGRAMA (para ver el efecto del bypass en vivo)
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: Math.round(parent.height * 0.44); color: "#000000"
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

        // Lienzo del grafo
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.bg1
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.line }

            // Cadena de nodos
            Row {
                anchors.centerIn: parent; spacing: 0
                Node {
                    title: "Fuente"; accent: Theme.purple; dimmed: !root.hasSel
                    subtitle: root.hasSel ? (TimelineModel.selIsTitle ? "Título" : (TimelineModel.selIsAudio ? "Audio" : "Clip")) : "—"
                    anchors.verticalCenter: parent.verticalCenter
                }
                Connector { live: root.hasSel }
                Node {
                    title: "Transformar"; accent: Theme.blue; bypassable: true; dimmed: !root.hasSel
                    bypassed: TimelineModel.selBypassTransform
                    subtitle: root.tfActive ? "pos·escala·rot" : "identidad"
                    onToggleBypass: TimelineModel.setSelBypassTransform(!TimelineModel.selBypassTransform)
                    onActivated: root.stageSelected(0)   // pestaña Inspector
                    anchors.verticalCenter: parent.verticalCenter
                }
                Connector { live: root.hasSel && !TimelineModel.selBypassTransform }
                Node {
                    title: "Color"; accent: Theme.green; bypassable: true; dimmed: !root.hasSel
                    bypassed: TimelineModel.selBypassColor
                    subtitle: root.colActive ? "graduado" : "neutro"
                    onToggleBypass: TimelineModel.setSelBypassColor(!TimelineModel.selBypassColor)
                    onActivated: root.stageSelected(2)   // pestaña Color
                    anchors.verticalCenter: parent.verticalCenter
                }
                Connector { live: root.hasSel && !TimelineModel.selBypassColor }
                Node {
                    title: "Salida"; accent: Theme.amber; dimmed: !root.hasSel
                    subtitle: root.hasSel ? "opacidad " + Math.round(TimelineModel.selOpacity * 100) + "%" : "—"
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Ayuda / sin selección
            Text {
                anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 16
                width: parent.width - 40; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                text: root.hasSel
                      ? "Pulsa ⏻ para omitir un nodo · pincha un nodo para editarlo en el Inspector"
                      : "Selecciona un clip en la línea de tiempo para ver su grafo de composición"
                color: Theme.textFaint; font.pixelSize: 10; font.family: Theme.sans
            }
        }
    }
}
