import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

/** Modal header: title + SVG close (no Segoe fonts). */
Item {
    id: root
    required property var theme
    property string title: ""
    property var dialog: null

    implicitHeight: 48
    Layout.fillWidth: true
    width: parent ? parent.width : implicitWidth

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: theme.spacing
        anchors.rightMargin: 4
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: root.title
            color: theme.textPrimary
            font.pixelSize: 16
            font.bold: true
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        Rectangle {
            Layout.preferredWidth: 36
            Layout.preferredHeight: 36
            radius: theme.radiusBtn
            color: closeArea.containsMouse ? (theme.darkMode ? "#c42b1c" : "#e81123")
                                           : "transparent"

            NyxIcon {
                anchors.centerIn: parent
                name: "close"
                width: 16
                height: 16
                opacity: closeArea.containsMouse ? 1.0 : 0.75
            }

            MouseArea {
                id: closeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.dialog)
                        root.dialog.close()
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: theme.border
    }
}
