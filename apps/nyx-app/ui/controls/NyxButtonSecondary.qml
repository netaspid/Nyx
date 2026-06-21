import QtQuick
import QtQuick.Controls

Button {
    id: ctrl
    required property var theme
    implicitHeight: 36
    padding: 10

    background: Rectangle {
        radius: theme.radiusBtn
        color: {
            if (!ctrl.enabled) return theme.inputBg
            if (ctrl.pressed) return theme.border
            if (ctrl.hovered) return theme.btnSecondaryHover
            return theme.btnSecondary
        }
        border.color: theme.border
        border.width: 1
    }

    contentItem: Text {
        text: ctrl.text
        color: ctrl.enabled ? theme.textPrimary : theme.textMuted
        font.pixelSize: Math.round(13 * theme.fontScale)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
