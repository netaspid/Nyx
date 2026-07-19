import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "."
import "../controls"

/** Карточка собеседника: ник, подпись, интересы, доступность. */
Popup {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn

    property string userId: ""
    property var info: ({})

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: theme.spacing
    width: Math.min(400, (parent ? parent.width : 480) - 48)
    implicitHeight: col.implicitHeight + padding * 2
    anchors.centerIn: parent
    dim: true

    function ensureOverlayParent() {
        const w = Window.window
        if (w && w.overlay)
            root.parent = w.overlay
        else if (Overlay.overlay)
            root.parent = Overlay.overlay
    }

    function openForPeer(uid) {
        userId = String(uid || "").trimmed().toLowerCase()
        ensureOverlayParent()
        node.refreshContactList()
        info = node.contactInfo(userId)
        open()
    }

    background: Rectangle {
        radius: theme.radiusBtn
        color: theme.bgSidebar
        border.color: theme.border
    }

    contentItem: ColumnLayout {
        id: col
        spacing: theme.spacing
        width: root.availableWidth

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Профиль")
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

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            AvatarBadge {
                size: 52
                label: info.nickname || "?"
                baseColor: avatarColorFn(info.nickname || "?")
                textColor: theme.textPrimary
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
                    text: info.availabilityLabel || ""
                    color: theme.online
                    font.pixelSize: 12
                }
                Label {
                    text: info.idShort || (userId ? userId.slice(0, 8) : "")
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
            text: qsTr("Пока нет подписи — она появится после следующего подключения, если собеседник заполнил профиль.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        NyxButton {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Открыть чат")
            onClicked: {
                node.openContact(userId)
                root.close()
            }
        }
    }
}
