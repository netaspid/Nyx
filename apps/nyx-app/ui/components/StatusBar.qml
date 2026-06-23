import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Нижняя строка статуса: соединение, hub, диагностика. */
Rectangle {
    id: root
    required property var theme
    property string text: ""
    property bool busy: false

    implicitHeight: 28
    color: theme.bgChatHeader
    border.color: theme.border

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: theme.spacing
        anchors.rightMargin: theme.spacing
        spacing: 8

        Rectangle {
            Layout.preferredWidth: 8
            Layout.preferredHeight: 8
            radius: 4
            color: busy ? theme.accent : theme.online
            visible: text.length > 0
        }

        Label {
            Layout.fillWidth: true
            text: text.length > 0 ? text : qsTr("Готов")
            color: text.length > 0 ? theme.textSecondary : theme.textMuted
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }
    }
}
