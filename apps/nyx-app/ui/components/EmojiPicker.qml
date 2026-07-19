import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Компактный выбор эмодзи для композера. */
Popup {
    id: root
    required property var theme
    signal emojiChosen(string emoji)

    modal: false
    focus: true
    padding: 8
    width: 280
    height: 200
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var emojis: [
        "😀", "😃", "😄", "😁", "😅", "😂", "🤣", "😊",
        "😇", "🙂", "😉", "😍", "🥰", "😘", "😋", "😜",
        "🤗", "🤔", "🤨", "😐", "😑", "😶", "🙄", "😏",
        "😣", "😥", "😮", "🤐", "😯", "😪", "😫", "🥱",
        "😴", "😌", "😛", "😝", "🤤", "😒", "😓", "😔",
        "👍", "👎", "👏", "🙌", "🤝", "✌️", "👌", "💪",
        "❤️", "🧡", "💛", "💚", "💙", "💜", "🖤", "💔",
        "🔥", "⭐", "✨", "🎉", "🎊", "💯", "✅", "❌",
        "📌", "📎", "📁", "💡", "⚡", "🚀", "🏠", "🌙"
    ]

    background: Rectangle {
        radius: theme.radiusBtn
        color: theme.bgSidebar
        border.color: theme.border
    }

    contentItem: GridView {
        id: grid
        clip: true
        cellWidth: 34
        cellHeight: 34
        model: root.emojis
        delegate: Item {
            width: grid.cellWidth
            height: grid.cellHeight
            required property string modelData
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: 6
                color: emoArea.containsMouse ? theme.btnSecondaryHover : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: modelData
                    font.pixelSize: 18
                }
                MouseArea {
                    id: emoArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.emojiChosen(modelData)
                        root.close()
                    }
                }
            }
        }
    }
}
