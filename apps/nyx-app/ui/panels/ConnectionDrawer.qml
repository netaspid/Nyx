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
    modal: false
    interactive: true
    enabled: node.sessionUnlocked

    Connections {
        target: node
        function onConnectionPanelOpenChanged() {
            if (node.connectionPanelOpen)
                root.open()
            else
                root.close()
        }
    }

    onOpenedChanged: {
        if (node.connectionPanelOpen !== opened)
            node.connectionPanelOpen = opened
    }

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

        Label {
            Layout.fillWidth: true
            visible: node.listening
            wrapMode: Text.WordWrap
            text: qsTr("Режим «Слушать» активен. На другом устройстве: Connect → token или LAN ниже.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: connTabs.implicitHeight + 8
            radius: theme.radiusBtn
            color: theme.inputBg
            border.color: theme.border

            TabBar {
                id: connTabs
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                background: Item {}

                TabButton {
                    id: listenTab
                    text: qsTr("Слушать")
                    width: (connTabs.width - connTabs.spacing * 2) / 3
                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: listenTab.checked ? theme.accent
                             : listenTab.hovered ? theme.btnSecondaryHover : "transparent"
                    }
                    contentItem: Label {
                        text: listenTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: listenTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 11
                        font.weight: listenTab.checked ? Font.DemiBold : Font.Normal
                    }
                }
                TabButton {
                    id: peerTab
                    text: qsTr("Peer")
                    width: (connTabs.width - connTabs.spacing * 2) / 3
                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: peerTab.checked ? theme.accent
                             : peerTab.hovered ? theme.btnSecondaryHover : "transparent"
                    }
                    contentItem: Label {
                        text: peerTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: peerTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 11
                        font.weight: peerTab.checked ? Font.DemiBold : Font.Normal
                    }
                }
                TabButton {
                    id: fieldTab
                    text: qsTr("Поле")
                    width: (connTabs.width - connTabs.spacing * 2) / 3
                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: fieldTab.checked ? theme.accent
                             : fieldTab.hovered ? theme.btnSecondaryHover : "transparent"
                    }
                    contentItem: Label {
                        text: fieldTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: fieldTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 11
                        font.weight: fieldTab.checked ? Font.DemiBold : Font.Normal
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 260
            currentIndex: connTabs.currentIndex

            ColumnLayout {
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    visible: node.inChat
                    wrapMode: Text.WordWrap
                    text: qsTr("Активна сессия (поле или чат). «Слушать» остановит её и откроет личный P2P.")
                    color: theme.textMuted
                    font.pixelSize: 11
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: node.inChat ? qsTr("Слушать (новая сессия)") : qsTr("Слушать")
                    onClicked: node.startListen()
                }
                NyxTextField {
                    id: inviteTokenField
                    Layout.fillWidth: true
                    theme: root.theme
                    readOnly: true
                    text: node.inviteToken
                    placeholderText: qsTr("Token после «Слушать»")
                    font.family: "Consolas"
                    font.pixelSize: 11
                }
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Копировать token")
                    enabled: node.inviteToken.length > 0
                    onClicked: node.copyInviteToken()
                }
            }

            ColumnLayout {
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Token для личного чата (не invite поля).")
                    color: theme.textMuted
                    font.pixelSize: 11
                }
                NyxTextField {
                    id: tokenField
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("64 hex token peer")
                    font.family: "Consolas"
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Подключиться к peer")
                    enabled: tokenField.text.trim().length > 0
                    onClicked: {
                        node.connectToken(tokenField.text)
                        root.close()
                    }
                }
            }

            ColumnLayout {
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Invite поля. Создатель должен нажать «Запустить» в «Поля».")
                    color: theme.textMuted
                    font.pixelSize: 11
                }
                NyxTextField {
                    id: fieldInviteField
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("64 hex invite поля")
                    font.family: "Consolas"
                    font.pixelSize: 11
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Войти в поле")
                    enabled: fieldInviteField.text.trim().length > 0
                    onClicked: {
                        node.joinField(fieldInviteField.text)
                        root.close()
                    }
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
                radius: root.theme.radiusBtn
                color: root.theme.btnSecondary
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    Label { text: instance; color: root.theme.textPrimary; Layout.fillWidth: true }
                    NyxButtonSecondary {
                        theme: root.theme
                        text: qsTr("Connect")
                        onClicked: { root.node.connectPeer(host, port) }
                    }
                }
            }
        }

        NyxTextField {
            Layout.fillWidth: true
            theme: root.theme
            text: node.rendezvousList
            placeholderText: "host:port или host:port,host2:port"
            onEditingFinished: {
                node.rendezvousList = text
                node.saveNetworkSettings()
            }
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
