import QtQuick
import QtQuick.Layouts
import PepeVideo

// Barra de título custom (ventana frameless).
Rectangle {
    id: root
    required property var win
    height: 32
    color: Theme.bg3

    // Arrastre de la ventana
    TapHandler {
        onGrabChanged: (transition, point) => {
            if (transition === PointerDevice.GrabExclusive) root.win.startSystemMove()
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        spacing: 9

        Rectangle {
            width: 16; height: 16; radius: 3
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "#e2a24b" }
                GradientStop { position: 1.0; color: "#c0632f" }
            }
        }
        Text { text: "PepeVideo Studio"; color: "#b9bcc4"; font.pixelSize: 12; font.weight: Font.DemiBold; font.family: Theme.sans }
        Text { text: "—"; color: Theme.textFaint; font.pixelSize: 12 }
        Text { text: "Viaje a Oaxaca · Vlog 04.lcproj"; color: "#8b8e97"; font.pixelSize: 12; font.family: Theme.sans }

        Item { Layout.fillWidth: true }

        // Controles de ventana
        Row {
            Layout.fillHeight: true
            Repeater {
                model: [
                    { key: "min",   hover: Theme.hover },
                    { key: "max",   hover: Theme.hover },
                    { key: "close", hover: Theme.red }
                ]
                delegate: Rectangle {
                    required property var modelData
                    width: 46; height: root.height
                    color: hovered.hovered ? modelData.hover : "transparent"
                    HoverHandler { id: hovered }
                    TapHandler {
                        onTapped: {
                            if (modelData.key === "min") root.win.showMinimized()
                            else if (modelData.key === "max") root.win.visibility === Window.Maximized ? root.win.showNormal() : root.win.showMaximized()
                            else root.win.close()
                        }
                    }
                    // Glifos
                    Rectangle { visible: modelData.key === "min"; anchors.centerIn: parent; width: 11; height: 1; color: "#a9acb4" }
                    Rectangle { visible: modelData.key === "max"; anchors.centerIn: parent; width: 10; height: 9; radius: 1; color: "transparent"; border.color: "#a9acb4"; border.width: 1 }
                    Item {
                        visible: modelData.key === "close"; anchors.centerIn: parent; width: 12; height: 12
                        Rectangle { anchors.centerIn: parent; width: 12; height: 1; color: "#c6c9d0"; rotation: 45 }
                        Rectangle { anchors.centerIn: parent; width: 12; height: 1; color: "#c6c9d0"; rotation: -45 }
                    }
                }
            }
        }
    }
}
