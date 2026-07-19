import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

/** Короткий invite-код + копирование полного hex. */
RowLayout {
    id: root
    required property var theme
    required property var node
    property string code: ""
    property string label: qsTr("Код приглашения")
    property string copyToast: qsTr("Код скопирован")

    spacing: 8

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Label {
            text: root.label
            color: theme.textMuted
            font.pixelSize: 10
            font.capitalization: Font.AllUppercase
        }
        Label {
            Layout.fillWidth: true
            text: code.length ? node.shortInviteCode(code) : qsTr("—")
            color: theme.textPrimary
            font.pixelSize: 13
            font.family: "Consolas"
            elide: Text.ElideMiddle
            ToolTip.visible: codeMouse.containsMouse && code.length > 0
            ToolTip.text: code
            MouseArea {
                id: codeMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (!code.length) return
                    node.copyToClipboard(code)
                }
            }
        }
    }

    NyxButton {
        theme: root.theme
        text: qsTr("Копировать")
        enabled: code.length > 0
        onClicked: {
            node.copyToClipboard(code)
        }
    }
}
