import QtQuick

/** Пузырь сообщения в чате (с анимацией появления). */
Item {
    id: bubbleRoot
    required property string author
    required property string messageText
    required property bool outgoing
    required property var timestamp
    required property real listWidth

    property var theme
    property var formatTime: function(ms) {
        if (!ms) return ""
        return Qt.formatTime(new Date(ms), "HH:mm")
    }

    readonly property int padH: theme ? theme.bubblePadH : 12
    readonly property int padV: theme ? theme.bubblePadV : 10
    readonly property real maxInnerW: listWidth * 0.72 - padH * 2

    width: listWidth
    height: bubble.implicitHeight + 10

    TextMetrics {
        id: metrics
        font.pixelSize: 15
        font.family: "Segoe UI"
        text: bubbleRoot.messageText
    }

    readonly property real innerW: Math.min(maxInnerW, Math.max(48, metrics.width))

    Rectangle {
        id: bubble
        anchors.right: bubbleRoot.outgoing ? parent.right : undefined
        anchors.left: bubbleRoot.outgoing ? undefined : parent.left
        anchors.rightMargin: theme ? theme.chatSideMargin : 14
        anchors.leftMargin: theme ? theme.chatSideMargin : 14
        implicitWidth: innerColumn.implicitWidth + padH * 2
        implicitHeight: innerColumn.implicitHeight + padV * 2
        width: implicitWidth
        height: implicitHeight
        opacity: 0
        y: 8
        color: bubbleRoot.outgoing
               ? (theme ? theme.bubbleOut : "#2b5278")
               : (theme ? theme.bubbleIn : "#2a3949")
        topLeftRadius: theme ? theme.radiusBubble : 16
        topRightRadius: theme ? theme.radiusBubble : 16
        bottomLeftRadius: bubbleRoot.outgoing
                          ? (theme ? theme.radiusBubble : 16)
                          : (theme ? theme.radiusBubbleTail : 4)
        bottomRightRadius: bubbleRoot.outgoing
                           ? (theme ? theme.radiusBubbleTail : 4)
                           : (theme ? theme.radiusBubble : 16)

        Component.onCompleted: appearAnimation.start()
        ParallelAnimation {
            id: appearAnimation
            NumberAnimation {
                target: bubble
                property: "opacity"
                from: 0
                to: 1
                duration: 150
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                target: bubble
                property: "y"
                from: 8
                to: 0
                duration: 150
                easing.type: Easing.OutCubic
            }
        }

        Column {
            id: innerColumn
            x: padH
            y: padV
            spacing: 4
            width: innerW

            Text {
                visible: !bubbleRoot.outgoing && bubbleRoot.author.length > 0
                text: bubbleRoot.author
                color: theme ? theme.accent : "#6ab2f2"
                font.bold: true
                font.pixelSize: 13
                width: parent.width
            }

            Text {
                text: bubbleRoot.messageText
                color: bubbleRoot.outgoing
                       ? (theme ? theme.bubbleTextOut : "#ffffff")
                       : (theme ? theme.bubbleTextIn : "#ffffff")
                font.pixelSize: 15
                font.family: "Segoe UI"
                wrapMode: Text.Wrap
                width: parent.width
            }

            Text {
                visible: bubbleRoot.timestamp > 0
                text: bubbleRoot.formatTime(bubbleRoot.timestamp)
                color: theme ? theme.textSecondary : "#99FFFFFF"
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
                width: parent.width
            }
        }
    }
}
