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
    width: Math.min(380, parent.width * 0.92)
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

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Сеть")
                color: theme.textPrimary
                font.pixelSize: 16
                font.bold: true
            }
            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: theme.radiusBtn
                color: closeArea.containsMouse ? (theme.darkMode ? "#c42b1c" : "#e81123")
                                               : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "\uE8BB"
                    font.family: "Segoe MDL2 Assets"
                    font.pixelSize: 12
                    color: closeArea.containsMouse ? "#ffffff" : theme.textSecondary
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Пригласите друга в личный чат или войдите в поле по коду.")
            color: theme.textMuted
            font.pixelSize: 12
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
                    id: inviteTab
                    text: qsTr("Пригласить")
                    width: (connTabs.width - connTabs.spacing * 2) / 3
                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: inviteTab.checked ? theme.accent
                             : inviteTab.hovered ? theme.btnSecondaryHover : "transparent"
                    }
                    contentItem: Label {
                        text: inviteTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: inviteTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 11
                        font.weight: inviteTab.checked ? Font.DemiBold : Font.Normal
                    }
                }
                TabButton {
                    id: chatTab
                    text: qsTr("Личный чат")
                    width: (connTabs.width - connTabs.spacing * 2) / 3
                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: chatTab.checked ? theme.accent
                             : chatTab.hovered ? theme.btnSecondaryHover : "transparent"
                    }
                    contentItem: Label {
                        text: chatTab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: chatTab.checked ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 11
                        font.weight: chatTab.checked ? Font.DemiBold : Font.Normal
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
            Layout.preferredHeight: 220
            currentIndex: connTabs.currentIndex

            // Пригласить: короткий код + копирование
            ColumnLayout {
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Скопируйте код и отправьте другу любым способом. Он вставит его во вкладке «Личный чат».")
                    color: theme.textSecondary
                    font.pixelSize: 12
                }
                InviteCodeRow {
                    Layout.fillWidth: true
                    theme: root.theme
                    node: root.node
                    code: node.dmInboxToken
                    label: qsTr("Ваш код для личного чата")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: node.dmInboxToken.length === 0
                    text: qsTr("Код появится после входа в аккаунт.")
                    color: theme.textMuted
                    font.pixelSize: 11
                }
            }

            // Войти в личный чат
            ColumnLayout {
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Вставьте код приглашения друга, чтобы открыть личный чат.")
                    color: theme.textSecondary
                    font.pixelSize: 12
                }
                NyxTextField {
                    id: tokenField
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("Код приглашения друга")
                    font.family: "Consolas"
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Подключиться")
                    enabled: tokenField.text.trim().length > 0
                    onClicked: {
                        node.connectToken(tokenField.text)
                        root.close()
                    }
                }
            }

            // Войти в поле
            ColumnLayout {
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Вставьте invite поля. Владелец должен держать эфир открытым.")
                    color: theme.textSecondary
                    font.pixelSize: 12
                }
                NyxTextField {
                    id: fieldInviteField
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("Вставьте invite (можно длинный hex)")
                    font.family: "Consolas"
                    font.pixelSize: 11
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Войти в эфир")
                    enabled: fieldInviteField.text.trim().length > 0
                    onClicked: {
                        node.joinField(fieldInviteField.text)
                        root.close()
                    }
                }
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Мои поля в сайдбаре")
                    onClicked: {
                        root.close()
                        node.sidebarMode = 2
                    }
                }
            }
        }

        Label {
            text: qsTr("Рядом в сети (LAN)")
            color: theme.textSecondary
            font.pixelSize: 11
            font.capitalization: Font.AllUppercase
        }

        ListView {
            id: lanList
            Layout.fillWidth: true
            Layout.preferredHeight: 120
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
                    Label {
                        text: instance
                        color: root.theme.textPrimary
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    NyxButtonSecondary {
                        theme: root.theme
                        text: qsTr("Связаться")
                        onClicked: root.node.connectPeer(host, port)
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: lanList.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Никого рядом в LAN.\nЗапустите Nyx на другом устройстве в той же сети.")
                color: theme.textMuted
                font.pixelSize: 11
            }
        }

        // Блок подсказок
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: tipsCol.implicitHeight + 16
            radius: theme.radiusBtn
            color: theme.inputBg
            border.color: theme.border

            ColumnLayout {
                id: tipsCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                spacing: 6

                Label {
                    text: qsTr("Подсказки")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: connTabs.currentIndex === 0
                          ? qsTr("Код приглашения постоянный: его можно сохранять и отправлять в мессенджере. Пока вы в приложении, друзья могут подключиться по этому коду.")
                          : (connTabs.currentIndex === 1
                             ? qsTr("Личный чат — переписка один на один и обмен файлами. Код берётся у друга во вкладке «Пригласить».")
                             : qsTr("Поле — групповой чат. Invite выдаёт создатель в разделе «Поля». Без активного hub у создателя войти нельзя."))
                    color: theme.textMuted
                    font.pixelSize: 11
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
