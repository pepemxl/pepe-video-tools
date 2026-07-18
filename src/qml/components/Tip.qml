import QtQuick
import QtQuick.Controls.Basic as C
import PepeVideo

// Tooltip reutilizable. Colócalo DENTRO de cualquier botón (Rectangle/Text/Item)
// y aparece al pasar el ratón. Trae su propio HoverHandler y no acepta eventos de
// puntero, así que NO interfiere con los TapHandler/HoverHandler/MouseArea del
// botón que lo contiene.
//
//   Rectangle { ...  Tip { text: "Añadir pista de vídeo" } }
//
// `edge` = "top" | "bottom" (por defecto) elige si aparece encima o debajo del
// botón — usa "top" para botones pegados al borde inferior de la ventana.
Item {
    id: root
    property string text: ""
    property int delay: 500
    property string edge: "bottom"
    anchors.fill: parent

    HoverHandler { id: hh }

    C.ToolTip {
        id: tip
        parent: root
        text: root.text
        visible: hh.hovered && root.text.length > 0
        delay: root.delay
        x: Math.round((root.width - width) / 2)
        y: root.edge === "top" ? -height - 6 : root.height + 6
        padding: 6
        font.pixelSize: 11
        font.family: Theme.sans
        background: Rectangle {
            color: Theme.panelHead
            border.color: Theme.line2
            border.width: 1
            radius: 5
        }
        contentItem: Text {
            text: tip.text
            color: Theme.textHi
            font: tip.font
        }
    }
}
