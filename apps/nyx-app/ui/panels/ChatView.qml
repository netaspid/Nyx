import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../controls"

ColumnLayout {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn
    required property var formatMsgTimeFn

    spacing: 0

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 56
        color: theme.bgChatHeader

        RowLayout {
            anchors.fill: parent
            anchors.margins: theme.spacing
            spacing: 12

            AvatarBadge {
                visible: node.peerTitle.length > 0
                size: 40
                label: node.peerTitle
                baseColor: avatarColorFn(node.peerTitle)
                textColor: theme.textPrimary
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: node.peerTitle.length ? node.peerTitle : qsTr("Выберите чат")
                    color: theme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Label {
                    visible: node.peerTitle.length > 0
                    text: node.inChat ? node.peerConnectionLabel : node.peerStatusText
                    color: node.inChat ? theme.online : theme.textSecondary
                    font.pixelSize: 12
                }
            }

            NyxTextField {
                id: msgSearch
                Layout.preferredWidth: 140
                visible: node.peerTitle.length > 0 && node.messages.count > 0
                theme: theme
                placeholderText: qsTr("Поиск")
                onTextChanged: node.searchMessages(text)
            }

            IconButton {
                visible: node.inChat
                theme: theme
                glyph: "\uE711"
                ToolTip.text: qsTr("Отключиться")
                onClicked: node.disconnectSession()
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.border
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        EmptyState {
            anchors.centerIn: parent
            width: parent.width * 0.7
            visible: !node.peerTitle.length && !node.inChat
            theme: theme
            emoji: "💬"
            title: qsTr("Выберите чат")
            hint: qsTr("Или откройте панель подключения")
        }

        ListView {
            id: msgList
            anchors.fill: parent
            anchors.margins: theme.chatSideMargin
            visible: node.peerTitle.length > 0 || node.inChat
            clip: true
            spacing: 6
            model: node.messages
            delegate: ChatBubble {
                width: msgList.width
                listWidth: msgList.width
                theme: theme
                author: model.author
                messageText: model.messageText
                outgoing: model.outgoing
                timestamp: model.timestamp
            }
            onCountChanged: Qt.callLater(function() { msgList.positionViewAtEnd() })
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        visible: node.peerTitle.length > 0 || node.inChat

        ProgressBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 4
            visible: node.fileProgressVisible
            from: 0
            to: 100
            value: node.fileProgressPercent
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: theme.bgInputBar

            RowLayout {
                anchors.fill: parent
                anchors.margins: theme.spacing
                spacing: 10

                IconButton {
                    theme: theme
                    glyph: "\uE16C"
                    enabled: node.inChat
                    ToolTip.text: qsTr("Файлы")
                    onClicked: node.connectionPanelOpen = true
                }

                NyxTextField {
                    id: msgField
                    Layout.fillWidth: true
                    theme: theme
                    enabled: node.canSendMessage
                    placeholderText: node.canSendMessage
                        ? qsTr("Сообщение (Ctrl+Enter)")
                        : qsTr("Подключитесь для отправки")
                    wrapMode: TextInput.Wrap
                    Keys.onPressed: function(event) {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && (event.modifiers & Qt.ControlModifier)) {
                            sendMsg()
                            event.accepted = true
                        }
                    }
                }

                Rectangle {
                    width: 44
                    height: 44
                    radius: 22
                    color: (node.canSendMessage && msgField.text.trim().length > 0)
                          ? (sendMouse.pressed ? theme.accentPress : theme.accent)
                          : theme.btnSecondary
                    NyxIcon {
                        anchors.centerIn: parent
                        name: "send"
                        opacity: parent.parent.parent.enabled ? 1 : 0.4
                    }
                    MouseArea {
                        id: sendMouse
                        anchors.fill: parent
                        enabled: node.canSendMessage && msgField.text.trim().length > 0
                        onClicked: sendMsg()
                    }
                }
            }
        }
    }

    function sendMsg() {
        if (!node.canSendMessage || msgField.text.trim().length === 0) return
        node.sendMessage(msgField.text)
        msgField.clear()
    }
}
