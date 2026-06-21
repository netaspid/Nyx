import QtQuick
import QtQuick.Controls

/** Пустое состояние: emoji + заголовок + подсказка. */
Column {
    id: root
    spacing: 8
    width: parent ? parent.width : 280

    property string emoji: "💬"
    property string title: ""
    property string hint: ""
    property var theme

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.emoji
        font.pixelSize: 48
    }
    Label {
        width: root.width
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: root.title
        color: theme ? theme.textPrimary : "#fff"
        font.pixelSize: 16
        font.bold: true
    }
    Label {
        width: root.width
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: root.hint
        color: theme ? theme.textSecondary : "#aaa"
        font.pixelSize: 13
    }
}
