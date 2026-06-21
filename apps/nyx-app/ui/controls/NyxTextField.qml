import QtQuick
import QtQuick.Controls

TextField {
    id: field
    required property var theme
    color: theme.textPrimary
    selectionColor: theme.accent
    selectedTextColor: theme.textPrimary
    placeholderTextColor: theme.textMuted
    padding: 10
    selectByMouse: true
    font.pixelSize: Math.round(14 * theme.fontScale)

    background: Rectangle {
        radius: theme.radiusInput
        color: theme.inputBg
        border.color: field.activeFocus ? theme.focusRing : theme.border
        border.width: field.activeFocus ? 2 : 1
    }
}
