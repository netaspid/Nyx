import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"

/** Профиль собеседника — Dialog на Overlay. */
Dialog {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    readonly property bool fullBleed: Qt.platform.os === "android"
                                      || (parent && parent.width < 720)

    modal: true
    standardButtons: Dialog.NoButton
    width: fullBleed ? (parent ? parent.width : Overlay.overlay.width) : (Math.min(400, parent ? parent.width - 48 : 400))
    height: fullBleed ? (parent ? parent.height : Overlay.overlay.height) : implicitHeight
    padding: theme.spacing
    x: fullBleed ? 0 : (parent ? Math.round((parent.width - width) / 2) : 0)
    y: fullBleed ? 0 : (parent ? Math.round((parent.height - height) / 2) : 0)

    property var info: ({})
    readonly property bool isSelf: !!(info.isSelf)

    onAboutToShow: {
        node.refreshContactList()
        info = node.contactInfo(node.peerInfoUserId)
    }

    background: Rectangle {
        color: theme.bgSidebar
        radius: root.fullBleed ? 0 : theme.radiusBtn
        border.color: theme.border
    }

    header: DialogChrome {
        theme: root.theme
        title: qsTr("Профиль")
        dialog: root
    }

    contentItem: ColumnLayout {
        spacing: theme.spacing
        width: parent ? parent.width : implicitWidth

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            AvatarBadge {
                size: 52
                label: info.nickname || "?"
                baseColor: avatarColorFn(info.nickname || "?")
                textColor: theme.textPrimary
                imageSource: info.avatarPath || node.peerAvatarPath(node.peerInfoUserId)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: info.nickname || qsTr("Собеседник")
                    color: theme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Label {
                    visible: root.isSelf
                    text: qsTr("Это вы")
                    color: theme.accent
                    font.pixelSize: 12
                }
                Label {
                    visible: !root.isSelf
                    text: info.availabilityLabel || ""
                    color: theme.online
                    font.pixelSize: 12
                }
                Label {
                    text: info.idShort || ""
                    color: theme.textMuted
                    font.pixelSize: 10
                    font.family: "Consolas"
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !!(info.bio && info.bio.length)
            wrapMode: Text.WordWrap
            text: info.bio || ""
            color: theme.textPrimary
            font.pixelSize: 13
        }

        Label {
            Layout.fillWidth: true
            visible: !!(info.interests && info.interests.length)
            wrapMode: Text.WordWrap
            text: qsTr("Интересы: %1").arg(info.interests || "")
            color: theme.textSecondary
            font.pixelSize: 12
        }

        Label {
            Layout.fillWidth: true
            visible: !(info.bio && info.bio.length) && !(info.interests && info.interests.length)
            wrapMode: Text.WordWrap
            text: qsTr("Подпись появится после следующего подключения, если собеседник заполнил профиль.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        Label {
            visible: peerPhotos.count > 0
            text: qsTr("Фото")
            color: theme.textSecondary
            font.pixelSize: 10
            font.capitalization: Font.AllUppercase
        }

        Flickable {
            Layout.fillWidth: true
            Layout.preferredHeight: peerPhotos.count > 0 ? 64 : 0
            visible: peerPhotos.count > 0
            contentWidth: peerPhotosRow.implicitWidth
            clip: true
            Row {
                id: peerPhotosRow
                spacing: 8
                Repeater {
                    id: peerPhotos
                    model: {
                        const fromInfo = info.photoPaths || []
                        if (fromInfo.length) return fromInfo
                        return node.peerAvatarHistory(node.peerInfoUserId)
                    }
                    delegate: AvatarBadge {
                        required property var modelData
                        size: 56
                        label: info.nickname || "?"
                        baseColor: avatarColorFn(info.nickname || "?")
                        imageSource: typeof modelData === "string" ? modelData : ""
                    }
                }
            }
        }

        NyxButton {
            Layout.fillWidth: true
            theme: root.theme
            visible: !root.isSelf
            text: qsTr("Открыть чат")
            onClicked: {
                node.openContact(node.peerInfoUserId)
                root.close()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.isSelf
            wrapMode: Text.WordWrap
            text: qsTr("Свой профиль и фото меняются в Настройках.")
            color: theme.textMuted
            font.pixelSize: 11
        }
    }

    footer: Item { implicitHeight: 4 }

    Connections {
        target: node
        function onPeerInfoOpenChanged() {
            if (node.peerInfoOpen)
                root.open()
            else
                root.close()
        }
    }

    onClosed: node.peerInfoOpen = false
}
