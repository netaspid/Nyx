import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

/** Сведения о поле: имя, invite, hub, roster участников. */
Popup {
    id: root
    required property var theme
    required property var node

    property string groupId: ""

    parent: Overlay.overlay

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: theme.spacing
    width: Math.min(420, parent ? parent.width - 48 : 420)
    implicitHeight: contentCol.implicitHeight + padding * 2
    anchors.centerIn: Overlay.overlay

    /** @param gid hex id поля из activeChatRefId или groupList. */
    function openForGroup(gid) {
        groupId = String(gid || "").trimmed().toLower()
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
        id: contentCol

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("О поле")
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
        spacing: theme.spacing
        width: root.availableWidth

        Label {
            Layout.fillWidth: true
            text: {
                const g = root.groupEntry()
                return g ? g.name : qsTr("Поле")
            }
            color: theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.groupEntry() !== null

            Label {
                text: root.groupEntry().isOwner ? qsTr("Создатель") : qsTr("Участник")
                color: root.groupEntry().isOwner ? theme.accent : theme.textMuted
                font.pixelSize: 11
                font.bold: root.groupEntry().isOwner
            }

            Label {
                text: qsTr("· %1 участн.").arg(root.groupEntry().memberCount || 0)
                color: theme.textMuted
                font.pixelSize: 11
            }

            Rectangle {
                visible: root.groupEntry().hubOnline
                radius: 4
                color: theme.online
                implicitWidth: hubLabel.implicitWidth + 12
                implicitHeight: 20
                Label {
                    id: hubLabel
                    anchors.centerIn: parent
                    text: qsTr("hub в сети")
                    color: "#ffffff"
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.groupEntry() && root.groupEntry().invite

            Label {
                Layout.fillWidth: true
                text: qsTr("invite: %1").arg(root.groupEntry().invite)
                color: theme.textSecondary
                font.pixelSize: 10
                font.family: "Consolas"
                elide: Text.ElideMiddle
            }

            NyxButtonSecondary {
                theme: root.theme
                text: qsTr("Копировать")
                onClicked: node.copyToClipboard(root.groupEntry().invite)
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Участники")
            color: theme.textSecondary
            font.pixelSize: 10
            font.capitalization: Font.AllUppercase
        }

        Repeater {
            model: root.groupEntry() ? root.groupEntry().members : []
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
            Layout.fillWidth: true
            visible: !root.groupEntry() || root.groupEntry().members.length === 0
            wrapMode: Text.WordWrap
            text: qsTr("Состав поля сохраняется локально. Участники остаются в списке после выхода из hub.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        NyxButtonSecondary {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Обновить список участников")
            onClicked: node.refreshFieldRoster()
        }
    }
}
