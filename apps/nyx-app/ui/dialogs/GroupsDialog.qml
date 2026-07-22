import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"

Dialog {
    id: root
    required property var theme
    required property var node

    readonly property bool fullBleed: Qt.platform.os === "android"
                                      || (parent && parent.width < 720)

    modal: true
    standardButtons: Dialog.NoButton
    width: fullBleed ? (parent ? parent.width : Overlay.overlay.width) : (Math.min(520, parent ? parent.width - 48 : 520))
    height: fullBleed ? (parent ? parent.height : Overlay.overlay.height) : (Math.min(580, parent ? parent.height - 80 : 580))
    padding: fullBleed ? theme.spacing : theme.spacing
    x: fullBleed ? 0 : (parent ? Math.round((parent.width - width) / 2) : 0)
    y: fullBleed ? 0 : (parent ? Math.round((parent.height - height) / 2) : 0)

    onAboutToShow: node.refreshGroupList()

    background: Rectangle {
        color: theme.bgSidebar
        radius: root.fullBleed ? 0 : theme.radiusBtn
        border.color: theme.border
    }

    header: DialogChrome {
        theme: root.theme
        title: qsTr("Поля")
        dialog: root
    }

    contentItem: ColumnLayout {
        spacing: theme.spacing
        width: parent ? parent.width : implicitWidth

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Создатель открывает эфир поля. Участники входят по invite, пока эфир открыт.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            NyxTextField {
                id: newFieldName
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Название нового поля")
            }

            NyxButton {
                theme: root.theme
                text: qsTr("Создать")
                enabled: newFieldName.text.trim().length > 0
                onClicked: {
                    node.createGroup(newFieldName.text)
                    newFieldName.clear()
                }
            }
        }

        Label {
            text: qsTr("Мои поля")
            color: theme.textSecondary
            font.pixelSize: 12
            font.capitalization: Font.AllUppercase
        }

        ListView {
            id: fieldsList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: node.groupList

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

                width: ListView.view.width
                height: content.implicitHeight + 16
                radius: theme.radiusBtn
                color: theme.btnSecondary
                border.color: hubOnline ? theme.online : theme.border
                border.width: hubOnline ? 1 : 0

                ColumnLayout {
                    id: content
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: name
                            color: theme.textPrimary
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            visible: hubOnline
                            radius: 4
                            color: theme.online
                            implicitWidth: statusLabel.implicitWidth + 12
                            implicitHeight: 20
                            Label {
                                id: statusLabel
                                anchors.centerIn: parent
                                text: qsTr("эфир открыт")
                                color: "#ffffff"
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: roleLabel
                            color: isOwner ? theme.accent : theme.textMuted
                            font.pixelSize: 11
                            font.bold: isOwner
                        }
                        Label {
                            text: qsTr("· %1 участн.").arg(memberCount)
                            color: theme.textMuted
                            font.pixelSize: 11
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: members && members.length > 0

                        Label {
                            text: qsTr("Участники")
                            color: theme.textSecondary
                            font.pixelSize: 10
                            font.capitalization: Font.AllUppercase
                        }

                        Repeater {
                            model: members
                            delegate: RowLayout {
                                required property string userId
                                required property string nickname
                                required property bool isOwner
                                required property string idShort

                                Layout.fillWidth: true
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: nickname + (isOwner ? qsTr(" · создатель") : "")
                                    color: theme.textPrimary
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: idShort
                                    color: theme.textMuted
                                    font.pixelSize: 10
                                    font.family: "Consolas"
                                }
                                NyxButtonSecondary {
                                    visible: fieldCard.isOwner && !isOwner
                                    theme: root.theme
                                    text: qsTr("Исключить")
                                    onClicked: node.removeFieldMember(groupId, userId)
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        NyxButton {
                            theme: root.theme
                            text: isOwner
                                  ? (hubOnline ? qsTr("Эфир открыт") : qsTr("Открыть эфир"))
                                  : qsTr("Войти в эфир")
                            enabled: isOwner ? !hubOnline : true
                            onClicked: {
                                if (isOwner) {
                                    node.startFieldHub(groupId)
                                } else {
                                    node.joinField(invite)
                                }
                            }
                        }
                        NyxButtonSecondary {
                            theme: root.theme
                            text: qsTr("Invite")
                            onClicked: node.copyToClipboard(invite)
                        }
                        NyxButtonSecondary {
                            theme: root.theme
                            text: qsTr("В чат")
                            onClicked: {
                                node.openConversation("group:" + groupId, 1, groupId, name, "")
                                root.close()
                            }
                        }
                        NyxButtonSecondary {
                            theme: root.theme
                            text: qsTr("Удалить")
                            onClicked: node.removeConversation("group:" + groupId)
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: fieldsList.count === 0
                text: qsTr("Пока нет полей")
                color: theme.textMuted
            }
        }

        Label {
            text: qsTr("Войти по invite")
            color: theme.textSecondary
            font.pixelSize: 12
            font.capitalization: Font.AllUppercase
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            NyxTextField {
                id: joinInvite
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("64 hex invite")
                font.family: "Consolas"
                font.pixelSize: 11
            }
            NyxButton {
                theme: root.theme
                text: qsTr("Подключиться")
                enabled: joinInvite.text.trim().length > 0
                onClicked: {
                    node.joinField(joinInvite.text)
                    joinInvite.clear()
                }
            }
        }
    }

    footer: Item { implicitHeight: 4 }

    Connections {
        target: node
        function onGroupsDialogOpenChanged() {
            if (node.groupsDialogOpen)
                root.open()
            else
                root.close()
        }
    }

    onClosed: node.groupsDialogOpen = false
}
