import QtQuick
import QtQuick.Controls.Basic as C
import PepeVideo

// Botón de la barra de menús: una etiqueta clicable que abre un menú desplegable.
// Los ítems se declaran como hijos (van al menú). `state` es un objeto compartido para
// coordinar que solo haya un menú abierto y que al pasar el ratón se cambie de menú.
Item {
    id: mb
    property string title
    property var state
    default property alias items: menu.contentData

    implicitWidth: lbl.width + 18
    implicitHeight: 24

    Rectangle {
        anchors.fill: parent; radius: 4
        color: (hover.hovered || menu.opened) ? Theme.hover : "transparent"
        Text {
            id: lbl; anchors.centerIn: parent; text: mb.title
            font.pixelSize: 12; font.family: Theme.sans
            color: (hover.hovered || menu.opened) ? "#ffffff" : "#b3b6be"
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: {
            // Si ya hay otro menú abierto, cambia a este al pasar el ratón.
            if (hovered && mb.state && mb.state.current && mb.state.current !== menu) {
                mb.state.current.close()
                menu.popup(0, mb.height)
            }
        }
    }
    TapHandler {
        onTapped: menu.opened ? menu.close() : menu.popup(0, mb.height)
    }

    C.Menu {
        id: menu
        padding: 5
        onOpened: if (mb.state) mb.state.current = menu
        onClosed: if (mb.state && mb.state.current === menu) mb.state.current = null
        background: Rectangle {
            implicitWidth: 240
            color: "#22242b"; radius: 6
            border.color: Theme.line2; border.width: 1
        }
    }
}
