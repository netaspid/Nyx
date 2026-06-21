import QtQuick
import QtQuick.Controls

Button {
    id: ctrl
    required property var theme
    implicitHeight: 38
    padding: 12

    background: Rectangle {
        radius: theme.radiusBtn
        color: {
            if (!ctrl.enabled) return theme.btnSecondary
            if (ctrl.pressed) return theme.accentPress
            if (ctrl.hovered) return theme.accentHover
            return theme.accent
        }
        border.width: ctrl.activeFocus ? 2 : 0
        border.color: theme.focusRing
    }

    contentItem: Text {
        text: ctrl.text
        color: ctrl.enabled ? theme.textPrimary : theme.textMuted
        font.pixelSize: Math.round(13 * theme.fontScale)
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
