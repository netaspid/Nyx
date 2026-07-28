import QtQuick
import QtQuick.Controls

AbstractButton {
    id: ctrl

    property string name: "settings"
    property string glyph: "" // legacy; ignored
    property color iconColor: theme ? theme.textPrimary : "#ffffff"
    property var theme
    property int btnSize: 36
    property bool active: false
    property bool accent: false
    property bool flat: false

    implicitWidth: btnSize
    implicitHeight: btnSize
    hoverEnabled: true
    checkable: false

    background: Rectangle {
        radius: btnSize / 2
        color: {
            if (!ctrl.enabled) return theme ? theme.inputBg : "#333"
            if (ctrl.accent) {
                if (ctrl.pressed) return theme ? theme.accentPress : "#4674a8"
                if (ctrl.hovered && Qt.platform.os !== "android")
                    return theme ? theme.accentHover : "#6a9fd4"
                return theme ? theme.accent : "#5288c1"
            }
            if (ctrl.active) return theme ? theme.accent : "#5288c1"
            if (ctrl.pressed) return theme ? theme.accentPress : "#4674a8"
            if (ctrl.hovered && Qt.platform.os !== "android")
                return theme ? theme.btnSecondaryHover : "#444"
            if (ctrl.flat) return "transparent"
            return theme ? theme.btnSecondary : "#2a2e38"
        }
        border.color: (ctrl.accent || ctrl.active || ctrl.flat)
                      ? "transparent"
                      : (theme ? theme.border : "#555")
        border.width: (ctrl.accent || ctrl.active || ctrl.flat) ? 0 : 1
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
