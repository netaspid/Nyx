import QtQuick
import QtQuick.Controls

MenuItem {
    id: root
    required property var theme

    implicitWidth: 208
    implicitHeight: visible ? 34 : 0
    leftPadding: 12
    rightPadding: 12
    topPadding: 4
    bottomPadding: 4

    contentItem: Text {
        text: root.text
        color: root.enabled ? theme.textPrimary : theme.textMuted
        opacity: root.enabled ? 1 : 0.55
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: root.highlighted ? theme.btnSecondaryHover : theme.bgSidebar
        radius: 5
    }
}
