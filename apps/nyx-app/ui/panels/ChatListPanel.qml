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
            Layout.bottomMargin: 8
            spacing: 8

            NyxLogo { theme: root.theme }
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
                imageSource: node.profileAvatarPath
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: node.profileNickname
                    color: theme.textPrimary
                    font.pixelSize: 15
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
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
                ToolTip.text: qsTr("Настройки")
                onClicked: root.settingsRequested()
            }
        }

        // Режимы списка — не «Файлы/Поля как вкладки экрана»
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: 8
            implicitHeight: modeTabs.implicitHeight + 8
            radius: theme.radiusBtn
            color: theme.inputBg
            border.color: theme.border

            TabBar {
                id: modeTabs
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                currentIndex: node.sidebarMode
                background: Item {}
                onCurrentIndexChanged: {
                    if (node.sidebarMode !== currentIndex)
                        node.sidebarMode = currentIndex
                }

                Repeater {
                    model: [qsTr("Чаты"), qsTr("Друзья"), qsTr("Поля")]
                    TabButton {
                        required property int index
                        required property string modelData
                        text: modelData
                        width: (modeTabs.width - modeTabs.spacing * 2) / 3
                        background: Rectangle {
                            radius: theme.radiusBtn - 2
                            color: parent.checked ? theme.accent
                                 : parent.hovered ? theme.btnSecondaryHover
                                 : "transparent"
                        }
                        contentItem: Label {
                            text: parent.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: parent.checked ? theme.textPrimary : theme.textSecondary
                            font.pixelSize: 12
                            font.weight: parent.checked ? Font.DemiBold : Font.Normal
                        }
                    }
                }
            }
        }

        Connections {
            target: node
            function onSidebarModeChanged() {
                if (modeTabs.currentIndex !== node.sidebarMode)
                    modeTabs.currentIndex = node.sidebarMode
            }
        }

        // Действия: пригласить / файлы
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: theme.spacing
            spacing: 8

            NyxButton {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("+ Связь")
                onClicked: node.connectionPanelOpen = true
            }
            NyxButtonSecondary {
                theme: root.theme
                text: qsTr("Файлы")
                onClicked: node.openFilesView()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: theme.spacing
            implicitHeight: chatSearch.implicitHeight
            visible: node.sidebarMode === 0 || node.sidebarMode === 1

            NyxTextField {
                id: chatSearch
                anchors.fill: parent
                theme: root.theme
                placeholderText: node.sidebarMode === 1 ? qsTr("Поиск друзей")
                                                       : qsTr("Поиск чатов")
                onTextChanged: listFilter.text = text
            }
        }

        QtObject { id: listFilter; property string text: "" }

        // —— Чаты ——
        ListView {
            id: chatListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: node.sidebarMode === 0
            model: node.chatList
            spacing: 2

            delegate: ChatListItem {
                width: chatListView.width
                theme: root.theme
                node: root.node
                avatarColorFn: root.avatarColorFn
                visible: listFilter.text.length === 0
                         || title.toLowerCase().indexOf(listFilter.text.toLowerCase()) >= 0
                         || preview.toLowerCase().indexOf(listFilter.text.toLowerCase()) >= 0
                onClicked: root.node.openConversation(key, kind, refId, title, lastSeen)
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: chatListView.count === 0
                theme: root.theme
                emoji: "💬"
                title: qsTr("Нет чатов")
                hint: qsTr("Нажмите «+ Связь», чтобы пригласить или войти по коду")
            }
        }

        // —— Друзья ——
        ListView {
            id: friendsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: node.sidebarMode === 1
            model: node.contactList
            spacing: 2

            delegate: Rectangle {
                id: friendRow
                required property string userId
                required property string nickname
                required property string idShort
                required property string lastSeen
                required property bool hasInvite
                required property string chatKey
                required property string sessionState
                required property string avatarPath

                width: friendsView.width
                height: 60
                color: friendMouse.containsMouse ? theme.btnSecondaryHover : "transparent"

                readonly property bool live: sessionState === "live"
                readonly property bool connecting: sessionState === "connecting"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 10

                    Item {
                        Layout.preferredWidth: 40
                        Layout.preferredHeight: 40
                        AvatarBadge {
                            anchors.fill: parent
                            size: 40
                            label: nickname
                            baseColor: avatarColorFn(nickname)
                            textColor: theme.textPrimary
                            imageSource: avatarPath
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
                        Label {
                            Layout.fillWidth: true
                            text: nickname
                            color: theme.textPrimary
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: live ? qsTr("на связи")
                                  : connecting ? qsTr("переподключение…")
                                  : lastSeen
                            color: live ? theme.online : theme.textSecondary
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        visible: !hasInvite && !live
                        text: qsTr("нет кода")
                        color: theme.textMuted
                        font.pixelSize: 10
                    }
                }

                MouseArea {
                    id: friendMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: node.openContact(userId)
                }

                visible: listFilter.text.length === 0
                         || nickname.toLowerCase().indexOf(listFilter.text.toLowerCase()) >= 0
                         || idShort.toLowerCase().indexOf(listFilter.text.toLowerCase()) >= 0
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: friendsView.count === 0
                theme: root.theme
                emoji: "👥"
                title: qsTr("Пока нет друзей")
                hint: qsTr("Контакты появятся после личного чата. Пригласите через «+ Связь».")
            }
        }

        // —— Поля ——
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: node.sidebarMode === 2
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: theme.spacing
                Layout.rightMargin: theme.spacing
                spacing: 6

                NyxTextField {
                    id: newFieldName
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("Название поля")
                }
                NyxTextField {
                    id: newFieldDesc
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("Описание (необязательно)")
                }
                NyxTextField {
                    id: newFieldDir
                    Layout.fillWidth: true
                    theme: root.theme
                    placeholderText: qsTr("Направление")
                }
                TagEditor {
                    id: newFieldTags
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(40, implicitHeight)
                    theme: root.theme
                    placeholderText: qsTr("Тег и пробел")
                }
                NyxCheckBox {
                    id: newFieldPublic
                    theme: root.theme
                    text: qsTr("Публичное (поиск на RV — позже)")
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Создать поле")
                    enabled: newFieldName.text.trim().length > 0
                    onClicked: {
                        node.createGroup(newFieldName.text, newFieldDesc.text, newFieldDir.text,
                                         newFieldTags.tagsText, newFieldPublic.checked)
                        newFieldName.clear()
                        newFieldDesc.clear()
                        newFieldDir.clear()
                        newFieldTags.tagsText = ""
                        newFieldPublic.checked = false
                        node.refreshGroupList()
                    }
                }
            }

            ListView {
                id: fieldsView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: node.groupList
                spacing: 6
                leftMargin: theme.spacing
                rightMargin: theme.spacing

                delegate: Rectangle {
                    id: fieldCard
                    required property string groupId
                    required property string name
                    required property string invite
                    required property bool isOwner
                    required property string roleLabel
                    required property int memberCount
                    required property bool hubOnline
                    required property var members
                    required property string tags
                    required property string description
                    required property string direction

                    width: fieldsView.width - fieldsView.leftMargin - fieldsView.rightMargin
                    radius: theme.radiusBtn
                    color: theme.inputBg
                    border.color: hubOnline ? theme.online : theme.border
                    border.width: hubOnline ? 1 : 1
                    implicitHeight: fieldCol.implicitHeight + 20

                    ColumnLayout {
                        id: fieldCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 10
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: name
                                color: theme.textPrimary
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                visible: hubOnline
                                radius: 4
                                color: theme.online
                                implicitWidth: etherLbl.implicitWidth + 10
                                implicitHeight: 18
                                Label {
                                    id: etherLbl
                                    anchors.centerIn: parent
                                    text: qsTr("эфир")
                                    color: "#ffffff"
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }
                        }

                        Label {
                            text: qsTr("%1 · %2 участн.").arg(roleLabel).arg(memberCount || 0)
                            color: theme.textMuted
                            font.pixelSize: 11
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: description.length > 0
                            text: description
                            color: theme.textSecondary
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: direction.length > 0
                            text: qsTr("Направление: %1").arg(direction)
                            color: theme.textMuted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }

                        TagEditor {
                            Layout.fillWidth: true
                            visible: tags.trim().length > 0
                            theme: root.theme
                            readOnly: true
                            tagsText: tags
                        }

                        InviteCodeRow {
                            Layout.fillWidth: true
                            theme: root.theme
                            node: root.node
                            code: invite
                            label: qsTr("Invite поля")
                            visible: isOwner && invite.length > 0
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            NyxButton {
                                Layout.fillWidth: true
                                theme: root.theme
                                text: isOwner
                                      ? (hubOnline ? qsTr("Открыть") : qsTr("Открыть эфир"))
                                      : qsTr("Войти")
                                onClicked: {
                                    if (isOwner) {
                                        if (hubOnline) {
                                            node.openConversation("group:" + groupId, 1, groupId, name, "")
                                            node.sidebarMode = 0
                                        } else {
                                            node.startFieldHub(groupId)
                                            node.sidebarMode = 0
                                        }
                                    } else {
                                        node.joinField(invite)
                                        node.sidebarMode = 0
                                    }
                                }
                            }
                            NyxButtonSecondary {
                                theme: root.theme
                                text: qsTr("Чаты")
                                onClicked: {
                                    node.openConversation("group:" + groupId, 1, groupId, name, "")
                                    node.sidebarMode = 0
                                }
                            }
                        }
                    }
                }

                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: fieldsView.count === 0
                    theme: root.theme
                    emoji: "📡"
                    title: qsTr("Нет полей")
                    hint: qsTr("Создайте поле выше или войдите по invite в «+ Связь»")
                }
            }
        }
    }

    Component.onCompleted: {
        if (node.sidebarMode === 1) node.refreshContactList()
        if (node.sidebarMode === 2) node.refreshGroupList()
    }
}
