import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"

/** Информация о поле и участники — в стиле приложения, клик → профиль. */
Dialog {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    modal: true
    standardButtons: Dialog.NoButton
    width: Math.min(460, parent ? parent.width - 48 : 460)
    height: Math.min(600, parent ? parent.height - 80 : 600)
    padding: 0

    readonly property string groupId: node.fieldInfoGroupId
    readonly property var groupListRef: node.groupList
    readonly property string inviteCode: String(node.fieldInfoInvite || "")
    readonly property bool isOwner: !!node.fieldInfoIsOwner
    readonly property var group: groupEntry()

    function groupEntry() {
        const id = String(groupId || "").trimmed().toLowerCase()
        const list = groupListRef
        for (let i = 0; i < list.length; ++i) {
            const g = list[i]
            if (String(g.groupId).trimmed().toLowerCase() === id)
                return g
        }
        return null
    }

    function loadEditorFromNode() {
        editDesc.text = node.fieldInfoDescription || ""
        editDir.text = node.fieldInfoDirection || ""
        editTags.tagsText = node.fieldInfoTags || ""
        editPublic.checked = !!node.fieldInfoPublicListed
    }

    function memberAvatar(userId) {
        const uid = String(userId || "").toLowerCase()
        if (uid === String(node.profileUserId || "").toLowerCase())
            return node.profileAvatarPath || ""
        return node.peerAvatarPath(uid) || ""
    }

    function openMemberProfile(userId) {
        const uid = String(userId || "").trimmed().toLowerCase()
        if (uid.length !== 64) return
        // PeerInfo поверх модалки поля
        node.openPeerInfo(uid)
    }

    onAboutToShow: {
        node.refreshFieldRoster()
        loadEditorFromNode()
    }

    background: Rectangle {
        color: theme.bgSidebar
        radius: theme.radiusBtn
        border.color: theme.border
    }

    header: DialogChrome {
        theme: root.theme
        title: qsTr("Поле и участники")
        dialog: root
    }

    contentItem: ColumnLayout {
        spacing: 0
        width: parent ? parent.width : implicitWidth

        // —— шапка поля ——
        Rectangle {
            Layout.fillWidth: true
            color: theme.bgChatHeader
            implicitHeight: headerCol.implicitHeight + theme.spacing * 2

            ColumnLayout {
                id: headerCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: theme.spacing
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        width: 40
                        height: 40
                        radius: 12
                        color: theme.accent
                        Label {
                            anchors.centerIn: parent
                            text: {
                                const n = root.group ? root.group.name : "?"
                                return n.length ? n[0].toUpperCase() : "?"
                            }
                            color: "#ffffff"
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: root.group ? root.group.name : qsTr("Поле")
                            color: theme.textPrimary
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            text: root.isOwner ? qsTr("Вы — создатель") : qsTr("Вы — участник")
                            color: theme.textMuted
                            font.pixelSize: 11
                        }
                    }

                    Rectangle {
                        visible: root.group && root.group.hubOnline
                        radius: 10
                        color: Qt.rgba(theme.online.r, theme.online.g, theme.online.b, 0.2)
                        border.color: theme.online
                        border.width: 1
                        implicitWidth: hubLabel.implicitWidth + 14
                        implicitHeight: 24
                        Label {
                            id: hubLabel
                            anchors.centerIn: parent
                            text: qsTr("эфир открыт")
                            color: theme.online
                            font.pixelSize: 11
                            font.weight: Font.Medium
                        }
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.border
            }
        }

        // —— прокручиваемое тело (gutter под скроллбар — без наезда на поля) ——
        Flickable {
            id: bodyFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.topMargin: theme.spacing
            clip: true
            readonly property int scrollGutter: contentHeight > height + 1 ? 14 : 0
            contentWidth: width
            contentHeight: bodyCol.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            interactive: contentHeight > height

            ScrollBar.vertical: ScrollBar {
                policy: bodyFlick.contentHeight > bodyFlick.height
                        ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                width: 10
                padding: 1
            }

            ColumnLayout {
                id: bodyCol
                width: bodyFlick.width - bodyFlick.scrollGutter
                spacing: theme.spacing

                    InviteCodeRow {
                        Layout.fillWidth: true
                        theme: root.theme
                        node: root.node
                        code: root.inviteCode
                        label: qsTr("Invite — отправьте другу")
                        visible: root.isOwner && root.inviteCode.length > 0
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.isOwner && root.inviteCode.length === 0
                        wrapMode: Text.WordWrap
                        text: qsTr("Invite не найден локально. Откройте эфир снова — или скопируйте код во вкладке «Поля».")
                        color: theme.offlineBadge
                        font.pixelSize: 11
                    }

                    // —— Просмотр меты (участник) ——
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: !root.isOwner

                        Label {
                            text: qsTr("О поле")
                            color: theme.textSecondary
                            font.pixelSize: 11
                            font.capitalization: Font.AllUppercase
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            radius: theme.radiusBtn
                            color: theme.inputBg
                            border.color: theme.border
                            border.width: 1
                            implicitHeight: metaCol.implicitHeight + 20

                            ColumnLayout {
                                id: metaCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: 10
                                spacing: 6

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: (node.fieldInfoDescription && String(node.fieldInfoDescription).length)
                                          ? node.fieldInfoDescription
                                          : qsTr("Описание не задано")
                                    color: (node.fieldInfoDescription && String(node.fieldInfoDescription).length)
                                           ? theme.textPrimary : theme.textMuted
                                    font.pixelSize: 13
                                    font.italic: !(node.fieldInfoDescription && String(node.fieldInfoDescription).length)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: !!(node.fieldInfoDirection && String(node.fieldInfoDirection).length)
                                    wrapMode: Text.WordWrap
                                    text: qsTr("Направление: %1").arg(node.fieldInfoDirection || "")
                                    color: theme.textSecondary
                                    font.pixelSize: 12
                                }
                                TagEditor {
                                    Layout.fillWidth: true
                                    theme: root.theme
                                    readOnly: true
                                    tagsText: node.fieldInfoTags || ""
                                    visible: !!(node.fieldInfoTags && String(node.fieldInfoTags).trim().length)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: !(node.fieldInfoTags && String(node.fieldInfoTags).trim().length)
                                    text: qsTr("Теги не заданы")
                                    color: theme.textMuted
                                    font.pixelSize: 12
                                    font.italic: true
                                }
                            }
                        }
                    }

                    // —— Редактирование меты (создатель) ——
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: root.isOwner

                        Label {
                            text: qsTr("Мете поля")
                            color: theme.textSecondary
                            font.pixelSize: 11
                            font.capitalization: Font.AllUppercase
                        }
                        NyxTextField {
                            id: editDesc
                            Layout.fillWidth: true
                            theme: root.theme
                            placeholderText: qsTr("Описание")
                        }
                        NyxTextField {
                            id: editDir
                            Layout.fillWidth: true
                            theme: root.theme
                            placeholderText: qsTr("Направление")
                        }
                        TagEditor {
                            id: editTags
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.max(40, implicitHeight)
                            theme: root.theme
                            readOnly: false
                            placeholderText: qsTr("Тег и пробел")
                        }
                        NyxCheckBox {
                            id: editPublic
                            theme: root.theme
                            text: qsTr("Публичное поле (задел на поиск)")
                        }
                        NyxButtonSecondary {
                            Layout.fillWidth: true
                            theme: root.theme
                            text: qsTr("Сохранить мету")
                            onClicked: {
                                node.updateGroupMeta(root.groupId, editDesc.text, editDir.text,
                                                     editTags.tagsText, editPublic.checked)
                                node.refreshFieldRoster()
                                root.loadEditorFromNode()
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: qsTr("Участники · %1").arg(node.fieldInfoMembers.length)
                        color: theme.textSecondary
                        font.pixelSize: 11
                        font.capitalization: Font.AllUppercase
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: node.fieldInfoMembers.length > 0
                        text: qsTr("Нажмите на участника, чтобы открыть профиль")
                        color: theme.textMuted
                        font.pixelSize: 11
                    }

                    Repeater {
                        model: node.fieldInfoMembers

                        delegate: Rectangle {
                            id: memberRow
                            // QVariantMap из fieldInfoMembers — через modelData
                            required property var modelData

                            readonly property string userId: String(modelData.userId || "")
                            readonly property string nickname: String(modelData.nickname || "?")
                            readonly property bool memberIsOwner: !!(modelData.isOwner)
                            readonly property string idShort: String(modelData.idShort || "")

                            Layout.fillWidth: true
                            implicitHeight: 56
                            radius: 12
                            color: memberMa.containsMouse ? theme.btnSecondaryHover : theme.inputBg
                            border.color: theme.border
                            border.width: 1

                            readonly property bool isSelf:
                                userId.toLowerCase() === String(node.profileUserId || "").toLowerCase()
                            readonly property string avatarSrc: {
                                const _c = node.contactList.length
                                const _p = node.profileAvatarPath
                                void _c; void _p
                                return root.memberAvatar(userId)
                            }

                            RowLayout {
                                id: memberRowLayout
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                AvatarBadge {
                                    size: 40
                                    label: memberRow.nickname
                                    baseColor: root.avatarColorFn(memberRow.nickname)
                                    textColor: theme.textPrimary
                                    imageSource: memberRow.avatarSrc
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Label {
                                            Layout.fillWidth: true
                                            text: memberRow.nickname
                                            color: theme.textPrimary
                                            font.pixelSize: 14
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        Rectangle {
                                            visible: memberRow.isSelf || memberRow.memberIsOwner
                                            radius: 6
                                            color: memberRow.isSelf
                                                   ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.22)
                                                   : theme.btnSecondary
                                            border.color: theme.border
                                            border.width: 1
                                            implicitWidth: roleLbl.implicitWidth + 10
                                            implicitHeight: 18
                                            Label {
                                                id: roleLbl
                                                anchors.centerIn: parent
                                                text: memberRow.isSelf
                                                      ? qsTr("вы")
                                                      : (memberRow.memberIsOwner ? qsTr("создатель") : "")
                                                color: memberRow.isSelf ? theme.accent : theme.textSecondary
                                                font.pixelSize: 10
                                            }
                                        }
                                    }
                                    Label {
                                        text: memberRow.idShort
                                        color: theme.textMuted
                                        font.pixelSize: 10
                                        font.family: "Consolas"
                                    }
                                }

                                NyxButtonSecondary {
                                    id: kickBtn
                                    visible: root.isOwner && !memberRow.memberIsOwner && !memberRow.isSelf
                                    theme: root.theme
                                    text: qsTr("Искл.")
                                    implicitHeight: 30
                                    onClicked: node.removeFieldMember(root.groupId, memberRow.userId)
                                }

                                Label {
                                    visible: !kickBtn.visible
                                    text: "\uE76C"
                                    font.family: "Segoe MDL2 Assets"
                                    font.pixelSize: 12
                                    color: theme.textMuted
                                }
                            }

                            // поверх строки, кроме кнопки «Искл.»
                            MouseArea {
                                id: memberMa
                                anchors.fill: parent
                                anchors.rightMargin: kickBtn.visible ? (kickBtn.width + 20) : 0
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                z: 2
                                onClicked: root.openMemberProfile(memberRow.userId)
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 48
                        visible: node.fieldInfoMembers.length === 0
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                        text: qsTr("Пока никого. Отправьте invite — участники появятся здесь.")
                        color: theme.textMuted
                        font.pixelSize: 12
                    }

                    Item { Layout.preferredHeight: 4 }
                }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: theme.border
        }

        NyxButtonSecondary {
            Layout.fillWidth: true
            Layout.margins: theme.spacing
            theme: root.theme
            text: qsTr("Обновить")
            onClicked: {
                node.refreshFieldRoster()
                if (root.isOwner)
                    root.loadEditorFromNode()
            }
        }
    }

    footer: Item { implicitHeight: 2 }

    Connections {
        target: node
        function onFieldInfoOpenChanged() {
            if (node.fieldInfoOpen) {
                root.loadEditorFromNode()
                root.open()
            } else {
                root.close()
            }
        }
    }

    onClosed: node.fieldInfoOpen = false
}
