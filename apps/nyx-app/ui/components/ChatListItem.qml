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
    property string sessionState: "idle"
    property bool selected: false

    readonly property bool offline: sessionState === "offline" || sessionState === "disconnected"
    readonly property bool live: sessionState === "live"
    readonly property bool connecting: sessionState === "connecting"
    // Offline: можно открыть историю, но не считать «в сети».
    readonly property bool selectable: true

    readonly property string fieldMemberHint: {
        if (kind !== 1) return ""
        for (let i = 0; i < node.groupList.length; ++i) {
            const g = node.groupList[i]
            if (String(g.groupId).toLowerCase() === String(refId).toLowerCase())
                return qsTr("%1 участн.").arg(g.memberCount || 0)
        }
        return ""
    }

    height: 64
    opacity: offline ? 0.45 : 1
    color: selected ? theme.btnSecondary : (mouseArea.containsMouse && selectable ? theme.btnSecondaryHover : "transparent")

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 10

        Item {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            AvatarBadge {
                anchors.fill: parent
                size: 44
                label: title
                baseColor: avatarColorFn(title)
                textColor: theme.textPrimary
            }

            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 10
                height: 10
                radius: 5
                visible: live || connecting
                color: connecting ? theme.accent : theme.online
                border.color: theme.bgSidebar
                border.width: 2
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: title
                    color: offline ? theme.textMuted : theme.textPrimary
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
                    text: offline ? qsTr("не в сети")
                          : (preview || (kind === 1 && fieldMemberHint.length
                                         ? fieldMemberHint
                                         : qsTr("Нет сообщений")))
                    color: theme.textSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: unread > 0 && !offline
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
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: selectable ? Qt.PointingHandCursor : Qt.ForbiddenCursor
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                contextMenu.popup()
                return
            }
            if (!selectable) return
            root.clicked()
        }
    }

    Menu {
        id: contextMenu
        MenuItem {
            text: qsTr("Отключиться")
            enabled: root.live || root.connecting
            onTriggered: node.disconnectChat(root.key)
        }
        MenuItem {
            text: qsTr("Копировать invite поля")
            visible: root.kind === 1
            onTriggered: {
                for (let i = 0; i < node.groupList.length; ++i) {
                    const g = node.groupList[i]
                    if (String(g.groupId).toLowerCase() === String(root.refId).toLowerCase()) {
                        node.copyToClipboard(g.invite)
                        return
                    }
                }
            }
        }
    }

    signal clicked()
}
