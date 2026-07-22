import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "../js/MarkdownFormat.js" as Md

/** Пузырь сообщения: блоки paragraph/table/formula/media/action. */
Item {
    id: bubbleRoot
    required property string author
    required property string messageText
    required property bool outgoing
    required property var timestamp
    required property real listWidth
    property var messageId: 0
    property string delivery: ""
    property string authorUserId: ""

    property var theme
    property var node: null
    property var formatTime: function(ms) {
        if (!ms) return ""
        return Qt.formatTime(new Date(ms), "HH:mm")
    }

    property var revealedSpoilers: ({})

    readonly property var blocks: Md.parseBlocks(messageText)
    readonly property bool isAction: blocks.length === 1 && blocks[0].type === "action"

    readonly property string deliveryMark: {
        if (!outgoing) return ""
        if (delivery === "pending") return "…"
        if (delivery === "failed") return "!"
        if (delivery === "delivered") return "✓"
        return ""
    }

    readonly property int padH: theme ? theme.bubblePadH : 12
    readonly property int padV: theme ? theme.bubblePadV : 10

    readonly property bool hasWideContent: {
        const bs = bubbleRoot.blocks
        for (let i = 0; i < bs.length; ++i) {
            if (bs[i].type === "code" || bs[i].type === "table")
                return true
        }
        return String(messageText || "").length > 500
    }

    readonly property real maxInnerW: listWidth * (hasWideContent ? 0.92 : 0.72) - padH * 2

    width: listWidth
    height: (isAction ? actionBubble.implicitHeight : bubble.implicitHeight) + 10

    function handleLink(link) {
        const s = String(link)
        if (s.indexOf("nyx-spoiler:") === 0) {
            const idx = parseInt(s.substring(12), 10)
            if (!isNaN(idx)) {
                const next = Object.assign({}, bubbleRoot.revealedSpoilers)
                next[idx] = true
                bubbleRoot.revealedSpoilers = next
            }
            return
        }
        if (s.indexOf("nyx-user:") === 0 && bubbleRoot.node) {
            bubbleRoot.node.openPeerInfo(s.substring(9))
            return
        }
        Qt.openUrlExternally(link)
    }

    function blockHtml(b) {
        if (!b) return ""
        if (b.type === "table") return Md.tableToHtml(b.text)
        if (b.type === "formula")
            return "<div style=\"text-align:center;margin:4px 0;\">" + Md.formulaToHtml(b.text) + "</div>"
        if (b.type === "media") return ""
        return Md.toHtml(b.text, bubbleRoot.revealedSpoilers)
    }

    // —— /me ——
    Rectangle {
        id: actionBubble
        visible: bubbleRoot.isAction
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 4
        width: Math.min(bubbleRoot.listWidth - 48, actionCol.implicitWidth + padH * 2)
        implicitHeight: actionCol.implicitHeight + padV * 2
        radius: theme ? theme.radiusBubble : 16
        color: theme ? theme.bubbleAction : "#243447"

        Column {
            id: actionCol
            x: padH
            y: padV
            spacing: 4
            width: Math.min(bubbleRoot.maxInnerW, 400)

            Text {
                width: parent.width
                wrapMode: Text.Wrap
                textFormat: Text.RichText
                color: theme ? theme.bubbleActionText : "#c5d4e8"
                font.pixelSize: 14
                font.italic: true
                text: {
                    const body = blocks[0] ? Md.escapeHtml(blocks[0].text) : ""
                    const nick = Md.escapeHtml(bubbleRoot.author || "?")
                    const accent = theme ? theme.accent : "#6ab2f2"
                    if (bubbleRoot.authorUserId.length)
                        return "<a href=\"nyx-user:" + bubbleRoot.authorUserId
                               + "\" style=\"color:" + accent
                               + ";text-decoration:none;font-weight:600;\">" + nick + "</a> " + body
                    return "<b>" + nick + "</b> " + body
                }
                onLinkActivated: bubbleRoot.handleLink(link)
            }

            RowLayout {
                width: parent.width
                Item { Layout.fillWidth: true }
                Text {
                    visible: bubbleRoot.timestamp > 0
                    text: bubbleRoot.formatTime(bubbleRoot.timestamp)
                    color: theme ? theme.textSecondary : "#99FFFFFF"
                    font.pixelSize: 11
                }
                Text {
                    visible: bubbleRoot.deliveryMark.length > 0
                    text: bubbleRoot.deliveryMark
                    color: theme ? theme.textSecondary : "#99FFFFFF"
                    font.pixelSize: 11
                }
            }
        }
    }

    // —— обычный ——
    Rectangle {
        id: bubble
        visible: !bubbleRoot.isAction
        anchors.right: bubbleRoot.outgoing ? parent.right : undefined
        anchors.left: bubbleRoot.outgoing ? undefined : parent.left
        anchors.rightMargin: theme ? theme.chatSideMargin : 14
        anchors.leftMargin: theme ? theme.chatSideMargin : 14
        implicitWidth: bubbleRoot.hasWideContent
                       ? (maxInnerW + padH * 2)
                       : Math.min(maxInnerW + padH * 2, Math.max(innerCol.implicitWidth + padH * 2, 64))
        implicitHeight: innerCol.implicitHeight + padV * 2
        color: bubbleRoot.outgoing
               ? (theme ? theme.bubbleOut : "#2b5278")
               : (theme ? theme.bubbleIn : "#2a3949")
        // Uniform radius: per-corner *Radius needs Qt 6.7+; Android kit is 6.5.3.
        radius: theme ? theme.radiusBubble : 16

        Column {
            id: innerCol
            x: padH
            y: padV
            spacing: 6
            width: maxInnerW

            Text {
                visible: !bubbleRoot.outgoing && bubbleRoot.author.length > 0
                text: bubbleRoot.author
                color: theme ? theme.accent : "#6ab2f2"
                font.bold: true
                font.pixelSize: 13
            }

            Repeater {
                model: bubbleRoot.blocks

                Column {
                    required property var modelData
                    width: bubbleRoot.maxInnerW
                    spacing: 4

                    // media
                    Item {
                        width: parent.width
                        height: mediaCol.visible ? mediaCol.implicitHeight : 0
                        visible: modelData.type === "media"

                        Column {
                            id: mediaCol
                            width: parent.width
                            spacing: 4
                            property string hash: modelData.hash || ""
                            property string caption: modelData.caption || ""
                            property string path: {
                                void mediaRefresh.tick
                                if (!bubbleRoot.node || !hash.length) return ""
                                return bubbleRoot.node.mediaLocalPath(hash)
                            }

                            Timer {
                                id: mediaRefresh
                                property int tick: 0
                                interval: 1200
                                running: mediaCol.visible && mediaCol.path.length === 0
                                repeat: true
                                onTriggered: {
                                    tick++
                                    if (bubbleRoot.node && mediaCol.hash.length && mediaCol.path.length === 0)
                                        bubbleRoot.node.ensureMediaAvailable(mediaCol.hash)
                                }
                            }

                            Component.onCompleted: {
                                if (bubbleRoot.node && hash.length && path.length === 0)
                                    bubbleRoot.node.ensureMediaAvailable(hash)
                            }

                            Image {
                                visible: mediaCol.path.length > 0
                                         && (!bubbleRoot.node || bubbleRoot.node.isImageMedia(mediaCol.hash))
                                source: mediaCol.path.length
                                        ? ("file:///" + String(mediaCol.path).replace(/\\/g, "/"))
                                        : ""
                                width: Math.min(parent.width, 280)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                sourceSize.width: 560
                            }

                            Rectangle {
                                visible: mediaCol.path.length > 0 && bubbleRoot.node
                                         && !bubbleRoot.node.isImageMedia(mediaCol.hash)
                                width: parent.width
                                height: 52
                                radius: 8
                                color: Qt.rgba(0.5, 0.5, 0.5, 0.2)
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    Label {
                                        Layout.fillWidth: true
                                        text: mediaCol.caption.length ? mediaCol.caption : qsTr("Видео")
                                        color: theme ? theme.textPrimary : "#fff"
                                        elide: Text.ElideRight
                                    }
                                    Button {
                                        text: qsTr("Открыть")
                                        onClicked: Qt.openUrlExternally(
                                            "file:///" + String(mediaCol.path).replace(/\\/g, "/"))
                                    }
                                }
                            }

                            Label {
                                visible: mediaCol.path.length === 0
                                text: qsTr("Загрузка медиа…")
                                color: theme ? theme.textSecondary : "#aaa"
                                font.pixelSize: 12
                            }

                            Label {
                                visible: mediaCol.caption.length > 0 && mediaCol.path.length > 0
                                text: mediaCol.caption
                                color: theme ? theme.textSecondary : "#aaa"
                                font.pixelSize: 12
                                width: parent.width
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    CodeBlockView {
                        visible: modelData.type === "code"
                        width: parent.width
                        lang: modelData.caption || ""
                        code: modelData.text || ""
                        maxContentHeight: 320
                    }

                    // paragraph with Telegram-spoilers
                    Flow {
                        visible: modelData.type === "paragraph"
                        width: parent.width
                        spacing: 3

                        Repeater {
                            model: modelData.type === "paragraph"
                                   ? Md.splitSpoilers(modelData.text) : []

                            Item {
                                id: segItem
                                required property var modelData
                                width: segInner.implicitWidth
                                height: segInner.implicitHeight

                                Item {
                                    id: segInner
                                    implicitWidth: spoiler.visible ? spoiler.implicitWidth
                                                   : mdText.implicitWidth
                                    implicitHeight: spoiler.visible ? spoiler.implicitHeight
                                                    : mdText.implicitHeight

                                    SpoilerSpan {
                                        id: spoiler
                                        visible: segItem.modelData.type === "spoiler"
                                        body: segItem.modelData.text || ""
                                        revealed: !!(bubbleRoot.revealedSpoilers[segItem.modelData.index])
                                        bubbleColor: bubbleRoot.outgoing
                                                     ? (theme ? theme.bubbleOut : "#2b5278")
                                                     : (theme ? theme.bubbleIn : "#2a3949")
                                        textColor: bubbleRoot.outgoing
                                                   ? (theme ? theme.bubbleTextOut : "#ffffff")
                                                   : (theme ? theme.bubbleTextIn : "#ffffff")
                                        maxWidth: bubbleRoot.maxInnerW
                                        onRevealRequested: {
                                            const next = Object.assign({}, bubbleRoot.revealedSpoilers)
                                            next[segItem.modelData.index] = true
                                            bubbleRoot.revealedSpoilers = next
                                        }
                                    }

                                    Text {
                                        id: mdText
                                        visible: segItem.modelData.type === "md"
                                        width: Math.min(bubbleRoot.maxInnerW,
                                                        Math.max(1, implicitWidth))
                                        text: Md.toHtml(segItem.modelData.text || "", {}, true)
                                        color: bubbleRoot.outgoing
                                               ? (theme ? theme.bubbleTextOut : "#ffffff")
                                               : (theme ? theme.bubbleTextIn : "#ffffff")
                                        font.pixelSize: 15
                                        font.family: "Segoe UI"
                                        wrapMode: Text.Wrap
                                        textFormat: Text.RichText
                                        linkColor: theme ? theme.accent : "#6ab2f2"
                                        onLinkActivated: bubbleRoot.handleLink(link)
                                    }
                                }
                            }
                        }
                    }

                    // table / formula
                    Text {
                        visible: modelData.type === "table" || modelData.type === "formula"
                        width: parent.width
                        text: bubbleRoot.blockHtml(modelData)
                        color: bubbleRoot.outgoing
                               ? (theme ? theme.bubbleTextOut : "#ffffff")
                               : (theme ? theme.bubbleTextIn : "#ffffff")
                        font.pixelSize: modelData.type === "table" ? 13 : 15
                        font.family: "Segoe UI"
                        wrapMode: Text.Wrap
                        textFormat: Text.RichText
                        linkColor: theme ? theme.accent : "#6ab2f2"
                        onLinkActivated: bubbleRoot.handleLink(link)
                    }
                }
            }

            RowLayout {
                width: parent.width
                Item { Layout.fillWidth: true }
                Text {
                    visible: bubbleRoot.timestamp > 0
                    text: bubbleRoot.formatTime(bubbleRoot.timestamp)
                    color: theme ? theme.textSecondary : "#99FFFFFF"
                    font.pixelSize: 11
                }
                Text {
                    visible: bubbleRoot.deliveryMark.length > 0
                    text: bubbleRoot.deliveryMark
                    color: bubbleRoot.delivery === "failed"
                           ? (theme ? theme.offlineBadge : "#e57373")
                           : (theme ? theme.textSecondary : "#99FFFFFF")
                    font.pixelSize: 11
                    font.bold: bubbleRoot.delivery === "failed"
                }
            }
        }
    }
}
