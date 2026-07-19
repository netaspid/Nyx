import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Цветное облачко тега (как в Jira). */
Rectangle {
    id: root
    required property var theme
    property string text: ""
    property color chipColor: theme.accent
    property bool removable: false

    signal removeRequested()

    readonly property color fg: {
        const c = Qt.color(chipColor)
        const luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b
        return luma > 0.55 ? "#1a1a1a" : "#ffffff"
    }

    implicitWidth: row.implicitWidth + 14
    implicitHeight: 24
    radius: height / 2
    color: Qt.rgba(chipColor.r, chipColor.g, chipColor.b, theme.darkMode ? 0.35 : 0.22)
    border.color: Qt.rgba(chipColor.r, chipColor.g, chipColor.b, 0.65)
    border.width: 1

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 4

        Label {
            text: root.text
            color: root.fg
            font.pixelSize: 11
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.maximumWidth: 140
        }

        Label {
            visible: root.removable
            text: "×"
            color: root.fg
            font.pixelSize: 14
            font.weight: Font.Bold
            opacity: removeMa.containsMouse ? 1 : 0.7
            MouseArea {
                id: removeMa
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.removeRequested()
            }
        }
    }
}
