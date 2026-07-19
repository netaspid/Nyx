import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "."
import "../controls"

/** Сведения о поле: invite и roster участников. */
Popup {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    property string groupId: ""

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: theme.spacing
    width: Math.min(440, (parent ? parent.width : 480) - 48)
    height: Math.min(520, (parent ? parent.height : 640) - 64)
    anchors.centerIn: parent
    dim: true

    function ensureOverlayParent() {
        // Popup внутри StackLayout иначе open() невидим — вешаем на overlay окна.
        const w = Window.window
        if (w && w.overlay)
            root.parent = w.overlay
        else if (Overlay.overlay)
            root.parent = Overlay.overlay
    }

    function openForGroup(gid) {
        groupId = String(gid || "").trimmed().toLowerCase()
        ensureOverlayParent()
        node.refreshFieldRoster()
        open()
    }

    onOpened: node.refreshFieldRoster()

    function groupEntry() {
        const id = groupId
        for (let i = 0; i < node.groupList.length; ++i) {
            const g = node.groupList[i]
            if (String(g.groupId).trimmed().toLowerCase() === id)
                return g
        }
        return null
    }

    background: Rectangle {
        radius: theme.radiusBtn
        color: theme.bgSidebar
        border.color: theme.border
    }

    contentItem: ColumnLayout {
        spacing: theme.spacing
        width: root.availableWidth

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Участники поля")
                color: theme.textPrimary
                font.pixelSize: 16
                font.bold: true
            }
            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: theme.radiusBtn
                color: fieldCloseArea.containsMouse ? (theme.darkMode ? "#c42b1c" : "#e81123")
                                                    : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "\uE8BB"
                    font.family: "Segoe MDL2 Assets"
                    font.pixelSize: 12
                    color: fieldCloseArea.containsMouse ? "#ffffff" : theme.textSecondary
                }
                MouseArea {
                    id: fieldCloseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: {
                const g = root.groupEntry()
                return g ? g.name : qsTr("Поле")
            }
            color: theme.accent
            font.pixelSize: 15
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.groupEntry() !== null

            Label {
                text: root.groupEntry().isOwner ? qsTr("Вы — создатель") : qsTr("Вы — участник")
                color: theme.textMuted
                font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                visible: root.groupEntry().hubOnline
                radius: 4
                color: theme.online
                implicitWidth: hubLabel.implicitWidth + 12
                implicitHeight: 20
                Label {
                    id: hubLabel
                    anchors.centerIn: parent
                    text: qsTr("эфир открыт")
                    color: "#ffffff"
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }

        InviteCodeRow {
            Layout.fillWidth: true
            theme: root.theme
            node: root.node
            code: root.groupEntry() ? root.groupEntry().invite : ""
            label: qsTr("Invite — отправьте другу")
            visible: root.groupEntry() && root.groupEntry().invite
        }

        Label {
            Layout.fillWidth: true
            visible: root.groupEntry() && root.groupEntry().description
            wrapMode: Text.WordWrap
            text: root.groupEntry() ? root.groupEntry().description : ""
            color: theme.textPrimary
            font.pixelSize: 12
        }
        Label {
            Layout.fillWidth: true
            visible: root.groupEntry() && (root.groupEntry().direction || root.groupEntry().tags)
            wrapMode: Text.WordWrap
            text: {
                const g = root.groupEntry()
                if (!g) return ""
                const parts = []
                if (g.direction) parts.push(qsTr("Направление: %1").arg(g.direction))
                if (g.tags) parts.push(qsTr("Теги: %1").arg(g.tags))
                return parts.join(" · ")
            }
            color: theme.textSecondary
            font.pixelSize: 11
        }
        Label {
            Layout.fillWidth: true
            visible: root.groupEntry() && root.groupEntry().publicListed
            text: qsTr("Публичное (поиск на rendezvous — позже)")
            color: theme.accent
            font.pixelSize: 11
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            visible: root.groupEntry() && root.groupEntry().isOwner

            Label {
                text: qsTr("Мете поля")
                color: theme.textSecondary
                font.pixelSize: 10
                font.capitalization: Font.AllUppercase
            }
            NyxTextField {
                id: editDesc
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Описание")
                text: root.groupEntry() ? (root.groupEntry().description || "") : ""
            }
            NyxTextField {
                id: editDir
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Направление")
                text: root.groupEntry() ? (root.groupEntry().direction || "") : ""
            }
            NyxTextField {
                id: editTags
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Теги через запятую")
                text: root.groupEntry() ? (root.groupEntry().tags || "") : ""
            }
            NyxCheckBox {
                id: editPublic
                theme: root.theme
                text: qsTr("Публичное поле (задел на поиск)")
                checked: root.groupEntry() ? !!root.groupEntry().publicListed : false
            }
            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Сохранить мету")
                onClicked: {
                    node.updateGroupMeta(root.groupId, editDesc.text, editDir.text, editTags.text,
                                         editPublic.checked)
                    node.refreshFieldRoster()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("В эфире · %1").arg(root.groupEntry() ? (root.groupEntry().memberCount || 0) : 0)
            color: theme.textSecondary
            font.pixelSize: 11
            font.capitalization: Font.AllUppercase
        }

        ListView {
            id: membersList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: root.groupEntry() ? root.groupEntry().members : []

            delegate: Rectangle {
                required property string userId
                required property string nickname
                required property bool isOwner
                required property string idShort

                width: membersList.width
                height: 52
                radius: theme.radiusBtn
                color: theme.inputBg

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10

                    AvatarBadge {
                        size: 36
                        label: nickname
                        baseColor: root.avatarColorFn(nickname)
                        textColor: theme.textPrimary
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: nickname + (isOwner ? qsTr(" · создатель") : "")
                            color: theme.textPrimary
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            text: idShort
                            color: theme.textMuted
                            font.pixelSize: 10
                            font.family: "Consolas"
                        }
                    }

                    NyxButtonSecondary {
                        visible: root.groupEntry()
                                 && root.groupEntry().isOwner
                                 && !isOwner
                        theme: root.theme
                        text: qsTr("Исключить")
                        onClicked: node.removeFieldMember(root.groupId, userId)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: membersList.count === 0
                width: parent.width - 16
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Пока никого. Отправьте invite — участники появятся здесь.")
                color: theme.textMuted
                font.pixelSize: 12
            }
        }

        NyxButtonSecondary {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Обновить")
            onClicked: node.refreshFieldRoster()
        }
    }
}
