import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../controls"

Drawer {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    edge: Qt.RightEdge
    width: Math.min(360, parent.width * 0.9)
    height: parent.height
    modal: true
    interactive: true
    visible: node.connectionPanelOpen
    onVisibleChanged: node.connectionPanelOpen = visible

    background: Rectangle { color: theme.bgSidebar }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: theme.spacing
        spacing: theme.spacing

        Label {
            text: qsTr("Подключение")
            color: theme.textPrimary
            font.pixelSize: 16
            font.bold: true
        }

        TabBar {
            id: connTabs
            Layout.fillWidth: true
            background: Rectangle { color: "transparent" }
            TabButton { text: qsTr("Слушать") }
            TabButton { text: qsTr("Connect") }
            TabButton { text: qsTr("Поля") }
            TabButton { text: qsTr("Файлы") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 240
            currentIndex: connTabs.currentIndex

            ColumnLayout {
                spacing: 8
                NyxButton {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Слушать")
                    enabled: !node.inChat
                    onClicked: node.startListen()
                }
                NyxTextField {
                    id: inviteTokenField
                    Layout.fillWidth: true
                    theme: theme
                    readOnly: true
                    text: node.inviteToken
                    placeholderText: qsTr("Token после «Слушать»")
                    font.family: "Consolas"
                    font.pixelSize: 11
                }
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Копировать token")
                    enabled: node.inviteToken.length > 0
                    onClicked: node.copyInviteToken()
                }
            }

            ColumnLayout {
                spacing: 8
                NyxTextField {
                    id: tokenField
                    Layout.fillWidth: true
                    theme: theme
                    placeholderText: qsTr("64 hex token")
                    font.family: "Consolas"
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Подключиться")
                    enabled: !node.inChat && tokenField.text.trim().length > 0
                    onClicked: {
                        node.connectToken(tokenField.text)
                        root.close()
                    }
                }
            }

            ColumnLayout {
                spacing: 8
                NyxTextField {
                    id: groupNameField
                    Layout.fillWidth: true
                    theme: theme
                    placeholderText: qsTr("Название поля")
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Создать поле")
                    onClicked: node.createGroup(groupNameField.text)
                }
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Копировать invite")
                    enabled: node.lastGroupInvite.length > 0
                    onClicked: node.copyLastGroupInvite()
                }
                NyxTextField {
                    id: groupInviteField
                    Layout.fillWidth: true
                    theme: theme
                    placeholderText: qsTr("group invite")
                    font.family: "Consolas"
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Войти в поле")
                    onClicked: {
                        node.joinGroup(groupInviteField.text)
                        root.close()
                    }
                }
            }

            ColumnLayout {
                spacing: 8
                NyxTextField {
                    id: folderPathField
                    Layout.fillWidth: true
                    theme: theme
                    placeholderText: qsTr("Путь к папке")
                }
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Индексировать")
                    onClicked: node.indexFolder(folderPathField.text)
                }
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Список файлов peer")
                    enabled: node.inChat
                    onClicked: node.requestRemoteFiles()
                }
                NyxTextField {
                    id: fileHashField
                    Layout.fillWidth: true
                    theme: theme
                    placeholderText: qsTr("SHA-256 hash")
                    enabled: node.inChat
                    font.family: "Consolas"
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: theme
                    text: qsTr("Скачать")
                    enabled: node.inChat && fileHashField.text.trim().length > 0
                    onClicked: node.downloadFile(fileHashField.text)
                }
            }
        }

        Label {
            text: qsTr("LAN peers")
            color: theme.textSecondary
            font.pixelSize: 11
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            clip: true
            model: node.lanPeers
            spacing: 4
            delegate: Rectangle {
                required property string instance
                required property string host
                required property int port
                width: ListView.view.width
                height: 48
                radius: theme.radiusBtn
                color: theme.btnSecondary
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    Label { text: instance; color: theme.textPrimary; Layout.fillWidth: true }
                    NyxButtonSecondary {
                        theme: theme
                        text: qsTr("Connect")
                        onClicked: { node.connectPeer(host, port); root.close() }
                    }
                }
            }
        }

        NyxTextField {
            Layout.fillWidth: true
            theme: theme
            text: node.rendezvous
            placeholderText: "rendezvous host:port"
            onEditingFinished: node.rendezvous = text
            font.family: "Consolas"
            font.pixelSize: 11
        }

        Label {
            Layout.fillWidth: true
            text: node.statusText
            wrapMode: Text.WordWrap
            color: theme.accent
            font.pixelSize: 11
        }

        Item { Layout.fillHeight: true }
    }
}
