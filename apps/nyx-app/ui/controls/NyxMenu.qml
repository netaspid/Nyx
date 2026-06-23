import QtQuick
import QtQuick.Controls

/** Контекстное меню в стиле темы (не системное белое). */
Menu {
    id: root
    required property var theme

    palette.window: theme.bgSidebar
    palette.windowText: theme.textPrimary
    palette.highlight: theme.accentPress
    palette.highlightedText: theme.textPrimary
    palette.button: theme.btnSecondary
    palette.buttonText: theme.textPrimary

    background: Rectangle {
        implicitWidth: 240
        color: theme.bgSidebar
        border.color: theme.border
        border.width: 1
        radius: theme.radiusBtn
    }

    delegate: MenuItem {
        id: menuItem
        implicitWidth: 240
        implicitHeight: 36
        padding: 10

        contentItem: Text {
            text: menuItem.text
            color: menuItem.enabled ? theme.textPrimary : theme.textMuted
            font.pixelSize: 13
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: menuItem.highlighted ? theme.btnSecondaryHover : "transparent"
            radius: 4
        }
    }
}
