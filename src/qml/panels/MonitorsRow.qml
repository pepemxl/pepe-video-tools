import QtQuick
import QtQuick.Layouts
import PepeVideo

// Centro: monitores Origen + Programa.
Rectangle {
    color: Theme.bg1

    RowLayout {
        anchors.fill: parent
        spacing: 1

        // ----- Monitor genérico -----
        component Monitor: ColumnLayout {
            property string title
            property color titleColor: Theme.textMid
            property string clipName
            property string zoomLabel
            property color screenBg
            property string caption
            property bool program: false
            property real fillW
            property string tcLeft
            property string tcRight
            property color tcLeftColor: Theme.textMid
            property var ctrl: null                         // VideoController o null
            readonly property bool live: ctrl && ctrl.hasMedia
            spacing: 0

            Rectangle {
                Layout.fillWidth: true; height: 30; color: Theme.panel3
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                    Text { text: title; color: titleColor; font.pixelSize: 11; font.weight: Font.DemiBold; font.family: Theme.sans }
                    Item { Layout.fillWidth: true }
                    Text { text: live ? ctrl.sourceName : clipName; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.sans; elide: Text.ElideMiddle; Layout.maximumWidth: 220 }
                    Item { Layout.fillWidth: true }
                    Text { text: zoomLabel; color: Theme.textDim; font.pixelSize: 10; font.family: Theme.sans }
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

                    // Vídeo real (monitor de origen)
                    VideoSurface {
                        anchors.fill: parent
                        visible: live
                        controller: ctrl
                    }

                    Text { visible: !live; anchors.centerIn: parent; text: caption; color: Theme.textFaint; font.pixelSize: 11; font.family: Theme.mono; font.letterSpacing: 1 }

                    // Caja de transformación (solo programa, sin vídeo)
                    Item {
                        visible: program && !live
                        anchors.fill: parent; anchors.margins: parent.width * 0.16
                        Rectangle { anchors.fill: parent; color: "transparent"; border.color: Theme.amber; border.width: 1.5
                            Rectangle { width: 9; height: 9; radius: 5; color: "transparent"; border.color: Theme.amber; border.width: 1.5; anchors.centerIn: parent } }
                        Repeater {
                            model: [[0,0],[1,0],[0,1],[1,1],[0.5,0],[0.5,1]]
                            delegate: Rectangle {
                                required property var modelData
                                width: 7; height: 7; color: Theme.amber
                                x: parent.width * modelData[0] - 3.5
                                y: parent.height * modelData[1] - 3.5
                            }
                        }
                    }
                    Rectangle {
                        visible: program && !live
                        anchors.left: parent.left; anchors.bottom: parent.bottom
                        anchors.leftMargin: parent.width * 0.16; anchors.bottomMargin: parent.height * 0.14
                        width: lt.width + 20; height: lt.height + 10; color: "#cc000000"
                        Rectangle { width: 3; height: parent.height; color: Theme.amber; anchors.left: parent.left }
                        Column { id: lt; anchors.centerIn: parent; spacing: 1
                            Text { text: "Mercado de Abastos"; color: "#ffffff"; font.pixelSize: 11; font.weight: Font.Bold; font.family: Theme.sans }
                            Text { text: "OAXACA DE JUÁREZ"; color: "#c6c9d0"; font.pixelSize: 8; font.letterSpacing: 1; font.family: Theme.sans } }
                    }
                    Rectangle { visible: program && !live; anchors.fill: parent; anchors.margins: parent.width * 0.05; color: "transparent"; border.color: "#14ffffff"; border.width: 1 }
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
                        readonly property real frac: live ? ctrl.fraction : fillW
                        Rectangle { height: parent.height; radius: 2; color: "#4a4d55"; width: scrub.width * scrub.frac }
                        Rectangle { width: 2; height: 10; color: Theme.amber; x: scrub.width * scrub.frac - 1; y: -3 }
                        TapHandler { enabled: live; onTapped: (p) => ctrl.seekFraction(p.position.x / scrub.width) }
                    }
                    // Timecodes + controles
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: live ? ctrl.positionTc : tcLeft; color: tcLeftColor; font.pixelSize: 11; font.family: Theme.mono }
                        Item { Layout.fillWidth: true }
                        Row {
                            spacing: 14
                            Text { text: "◀◀"; color: Theme.text; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                            Rectangle {
                                visible: program; width: 24; height: 24; radius: 12; color: Theme.amber; anchors.verticalCenter: parent.verticalCenter
                                Text { anchors.centerIn: parent; text: (live && ctrl.playing) ? "❚❚" : "▶"; color: Theme.amberInk; font.pixelSize: 12 }
                                TapHandler { onTapped: if (live) ctrl.togglePlay() }
                            }
                            Text { visible: !program; text: (live && ctrl.playing) ? "❚❚" : "▶"; color: Theme.text; font.pixelSize: 15; anchors.verticalCenter: parent.verticalCenter
                                   TapHandler { onTapped: if (live) ctrl.togglePlay() } }
                            Text { text: "▶▶"; color: Theme.text; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                            Text { visible: program; text: "●"; color: Theme.red; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                        }
                        Item { Layout.fillWidth: true }
                        Text { text: live ? ctrl.durationTc : tcRight; color: Theme.textDim; font.pixelSize: 11; font.family: Theme.mono }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Monitor {
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "ORIGEN"; clipName: "—"; zoomLabel: "Ajustar ▾"
            screenBg: "#20272e"; caption: "SIN CLIP · doble clic en un medio"; program: false
            fillW: 0.34; tcLeft: "00:00:00:00"; tcRight: "00:00:00:00"
            ctrl: VideoController
        }
        Monitor {
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "PROGRAMA"; titleColor: Theme.amber; clipName: "Timeline · Vlog 04"; zoomLabel: "100% ▾"
            screenBg: "#2a2f26"; caption: "MERCADO · PUESTO"; program: true
            fillW: 0.52; tcLeft: "00:04:12:08"; tcLeftColor: Theme.amber; tcRight: "00:08:03:14"
        }
    }
}
