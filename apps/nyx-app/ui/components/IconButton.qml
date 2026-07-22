import QtQuick
import QtQuick.Controls

/** Circular button with SVG icon from qrc:/icons (no Windows-only fonts). */
AbstractButton {
    id: ctrl

    property string name: "settings"
    property string glyph: "" // legacy; ignored
    property color iconColor: theme ? theme.textPrimary : "#ffffff"
    property var theme
    property int btnSize: 36

    implicitWidth: btnSize
    implicitHeight: btnSize
    hoverEnabled: true

    background: Rectangle {
        radius: btnSize / 2
        color: {
            if (!ctrl.enabled) return theme ? theme.inputBg : "#333"
            if (ctrl.pressed) return theme ? theme.accentPress : "#4674a8"
            if (ctrl.hovered) return theme ? theme.btnSecondaryHover : "#444"
            return "transparent"
        }
        border.color: theme ? theme.border : "#555"
        border.width: ctrl.enabled ? 0 : 1
    }

    contentItem: Item {
        NyxIcon {
            anchors.centerIn: parent
            name: ctrl.name
            width: Math.round(ctrl.btnSize * 0.48)
            height: width
            opacity: ctrl.enabled ? 1.0 : 0.4
        }
    }
}
