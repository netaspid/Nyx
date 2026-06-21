import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../controls"
import "../dialogs"

Rectangle {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    color: theme.bgSidebar

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: theme.spacing
            Layout.bottomMargin: 4
            spacing: 8

            NyxLogo {
                theme: root.theme
            }

            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: theme.spacing
            spacing: 8

            AvatarBadge {
                size: 36
                label: node.profileNickname
                baseColor: avatarColorFn(node.profileNickname)
                textColor: theme.textPrimary
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: node.profileNickname
                    color: theme.textPrimary
                    font.pixelSize: 15
                    font.bold: true
                }
                Label {
                    text: "id: " + node.profileIdShort
                    color: theme.textSecondary
                    font.pixelSize: 11
                }
            }

            IconButton {
                theme: theme
                glyph: "\uE710"
                ToolTip.visible: hovered
                ToolTip.text: "Подключение"
                onClicked: node.connectionPanelOpen = true
            }

            IconButton {
                theme: theme
                glyph: "\uE713"
                onClicked: settingsDialog.open()
            }
        }

        NyxTextField {
            id: chatSearch
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            theme: theme
            placeholderText: "Поиск чатов"
            onTextChanged: chatFilter.text = text
        }

        QtObject { id: chatFilter; property string text: "" }

        ListView {
            id: chatListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: node.chatList
            spacing: 2

            delegate: ChatListItem {
                width: chatListView.width
                theme: root.theme
                node: root.node
                avatarColorFn: root.avatarColorFn
                title: model.title
                preview: model.preview
                timeLabel: model.timeLabel
                unread: model.unread
                kind: model.kind
                visible: chatFilter.text.length === 0
                         || title.toLowerCase().indexOf(chatFilter.text.toLowerCase()) >= 0
                         || preview.toLowerCase().indexOf(chatFilter.text.toLowerCase()) >= 0
                onClicked: node.openConversation(model.key, model.kind, model.refId, model.title, model.lastSeen)
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: chatListView.count === 0
                theme: theme
                emoji: "💬"
                title: "Нет чатов"
                hint: "Подключитесь к peer или создайте поле"
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
        theme: root.theme
        node: root.node
    }
}
