import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Rectangle {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn
    required property string title
    required property string preview
    required property string timeLabel
    required property int unread
    required property int kind
    required property string key
    required property string refId
    required property string lastSeen

    property bool selected: false

    height: 64
    color: selected ? theme.btnSecondary : (mouseArea.containsMouse ? theme.btnSecondaryHover : "transparent")

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        AvatarBadge {
            size: 44
            label: title
            baseColor: avatarColorFn(title)
            textColor: theme.textPrimary
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: title
                    color: theme.textPrimary
                    font.pixelSize: Math.round(14 * theme.fontScale)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Label {
                    text: timeLabel
                    color: theme.textMuted
                    font.pixelSize: 11
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: preview || qsTr("Нет сообщений")
                    color: theme.textSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: unread > 0
                    Layout.preferredWidth: Math.max(18, unreadLabel.implicitWidth + 8)
                    Layout.preferredHeight: 18
                    radius: 9
                    color: theme.accent
                    Label {
                        id: unreadLabel
                        anchors.centerIn: parent
                        text: unread > 99 ? "99+" : unread
                        color: theme.textPrimary
                        font.pixelSize: 10
                        font.bold: true
                    }
                }
            }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }

    signal clicked()
}
