import QtQuick
import QtQuick.Controls

Menu {
    id: root
    required property var theme
    property real menuWidth: 208

    palette.window: theme.bgSidebar
    palette.windowText: theme.textPrimary
    palette.highlight: theme.accentPress
    palette.highlightedText: theme.textPrimary
    palette.button: theme.btnSecondary
    palette.buttonText: theme.textPrimary

    background: Rectangle {
        implicitWidth: root.menuWidth
        color: theme.bgSidebar
        border.color: theme.border
        border.width: 1
        radius: theme.radiusBtn
    }

    delegate: MenuItem {
        id: menuItem
        implicitWidth: root.menuWidth
        implicitHeight: 34
        leftPadding: 12
        rightPadding: 12
        topPadding: 4
        bottomPadding: 4

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
