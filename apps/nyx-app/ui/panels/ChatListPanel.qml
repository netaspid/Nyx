import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"
import "../controls"

Rectangle {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    signal settingsRequested()

    color: theme.bgSidebar

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: theme.spacing
            Layout.bottomMargin: theme.spacing
            spacing: 8

            NyxLogo {
                theme: root.theme
            }

            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: theme.spacing
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
                theme: root.theme
                glyph: "\uE713"
                onClicked: root.settingsRequested()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: theme.spacing
            implicitHeight: sidebarTabs.implicitHeight + 8
            radius: theme.radiusBtn
            color: theme.inputBg
            border.color: theme.border

            TabBar {
                id: sidebarTabs
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                background: Item {}

                TabButton {
                    id: fieldsTab
                    text: qsTr("Поля")
                    width: (sidebarTabs.width - sidebarTabs.spacing) / 2
                    onClicked: node.openGroupsDialog()

                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: fieldsTab.checked ? theme.accent
                             : fieldsTab.hovered ? theme.btnSecondaryHover
                             : "transparent"
                    }
                    contentItem: Label {
                        text: fieldsTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: fieldsTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 13
                        font.weight: fieldsTab.checked ? Font.DemiBold : Font.Normal
                    }
                }

                TabButton {
                    id: connectionTab
                    text: qsTr("Подключение")
                    width: (sidebarTabs.width - sidebarTabs.spacing) / 2
                    onClicked: node.connectionPanelOpen = !node.connectionPanelOpen

                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: connectionTab.checked ? theme.accent
                             : connectionTab.hovered ? theme.btnSecondaryHover
                             : "transparent"
                    }
                    contentItem: Label {
                        text: connectionTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: connectionTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 13
                        font.weight: connectionTab.checked ? Font.DemiBold : Font.Normal
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: theme.spacing
            implicitHeight: chatSearch.implicitHeight

            NyxTextField {
                id: chatSearch
                anchors.fill: parent
                theme: root.theme
                placeholderText: qsTr("Поиск чатов")
                onTextChanged: chatFilter.text = text
            }
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
                visible: chatFilter.text.length === 0
                         || title.toLowerCase().indexOf(chatFilter.text.toLowerCase()) >= 0
                         || preview.toLowerCase().indexOf(chatFilter.text.toLowerCase()) >= 0
                onClicked: root.node.openConversation(key, kind, refId, title, lastSeen)
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: chatListView.count === 0
                theme: root.theme
                emoji: "💬"
                title: qsTr("Нет чатов")
                hint: qsTr("Подключитесь к peer или создайте поле")
            }
        }
    }

    Connections {
        target: node
        function onConnectionPanelOpenChanged() {
            connectionTab.checked = node.connectionPanelOpen
        }
        function onGroupsDialogOpenChanged() {
            fieldsTab.checked = node.groupsDialogOpen
        }
    }
}
