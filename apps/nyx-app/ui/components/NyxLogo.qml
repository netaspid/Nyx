import QtQuick
import QtQuick.Controls

/** Бренд Nyx: SVG-логотип (луна + подпись). */
Item {
    id: root
    property var theme
    property bool compact: false
    property color textColor: theme ? theme.textPrimary : "#ffffff"

    implicitWidth: compact ? mark.width : logo.width
    implicitHeight: compact ? mark.height : logo.height

    Image {
        id: mark
        visible: root.compact
        source: "qrc:/icons/nyx-mark.svg"
        sourceSize.width: 32
        sourceSize.height: 32
        width: 32
        height: 32
        fillMode: Image.PreserveAspectFit
    }

    Image {
        id: logo
        visible: !root.compact
        source: "qrc:/icons/nyx-logo.svg"
        sourceSize.width: 120
        sourceSize.height: 32
        width: 120
        height: 32
        fillMode: Image.PreserveAspectFit
        color: root.textColor
    }
}
