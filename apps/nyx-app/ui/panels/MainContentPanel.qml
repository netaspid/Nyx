import QtQuick
import QtQuick.Layouts

Item {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn
    required property var formatMsgTimeFn

    StackLayout {
        anchors.fill: parent
        currentIndex: node.mainViewMode

        ChatView {
            theme: root.theme
            node: root.node
            avatarColorFn: root.avatarColorFn
            formatMsgTimeFn: root.formatMsgTimeFn
        }

        FilesPanel {
            theme: root.theme
            node: root.node
        }
    }
}
