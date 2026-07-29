import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Effects
import QtMultimedia
import "."
import "../js/MarkdownFormat.js" as Md

/** Пузырь сообщения: блоки paragraph/table/formula/media/action. */
Item {
    id: bubbleRoot
    required property string author
    required property string messageText
    required property bool outgoing
    required property var timestamp
    required property real listWidth
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

    function requestContextMenu(item, position) {
        const p = item.mapToItem(Overlay.overlay, position.x, position.y)
        bubbleRoot.contextRequested(Md.toPlainText(messageText), p.x, p.y)
    }

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

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function(eventPoint) {
                bubbleRoot.requestContextMenu(actionBubble, eventPoint.position)
            }
        }

        TapHandler {
            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
            onLongPressed: bubbleRoot.requestContextMenu(actionBubble, point.position)
        }

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

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: function(eventPoint) {
                bubbleRoot.requestContextMenu(bubble, eventPoint.position)
            }
        }

        TapHandler {
            id: touchContextHandler
            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
            onLongPressed: bubbleRoot.requestContextMenu(bubble, point.position)
        }

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

                    Column {
                        id: fileBlock
                        width: parent.width
                        spacing: 6
                        visible: modelData.type === "file"
                        property string hash: modelData.hash || ""
                        property string fileName: modelData.caption || qsTr("Файл")
                        property string mime: modelData.mime || "application/octet-stream"
                        property var fileSize: modelData.size || 0
                        property string folderRoot: modelData.root || ""
                        property string folderRel: modelData.rel || ""
                        property bool requested: false
                        property bool isDirectory: mime === "application/x-nyx-directory"
                        property bool isText: !isDirectory
                                              && (mime.indexOf("text/") === 0
                                                  || mime === "application/json")
                        property bool isAudio: !isDirectory && mime.indexOf("audio/") === 0
                        property bool isVoice: isAudio
                                               && (fileName.toLowerCase()
                                                   .indexOf("voice-message") === 0
                                                   || fileName.toLowerCase() === "voice.m4a")
                        property bool isVideo: !isDirectory && mime.indexOf("video/") === 0
                        property bool isImage: !isDirectory && mime.indexOf("image/") === 0
                        property bool isCircle: isVideo
                                                && fileName.toLowerCase()
                                                    .indexOf("circle-message") === 0
                        property string localPath: {
                            void fileRefresh.tick
                            return bubbleRoot.node && hash.length && !isDirectory
                                   ? bubbleRoot.node.fileLocalPath(hash) : ""
                        }
                        property int syncState: {
                            void fileRefresh.tick
                            return bubbleRoot.node && hash.length
                                   ? bubbleRoot.node.fileSyncState(hash) : 0
                        }
                        property string previewText: localPath.length && isText
                                                     ? bubbleRoot.node.fileTextPreview(hash) : ""

                        Component.onCompleted: {
                            if (!isDirectory
                                    && (isImage || isVoice || isCircle
                                        || (isText && fileSize <= 262144))
                                    && bubbleRoot.node) {
                                requested = true
                                bubbleRoot.node.ensureFileAvailable(hash, fileName)
                            }
                        }

                        Timer {
                            id: fileRefresh
                            property int tick: 0
                            interval: 900
                            repeat: true
                            running: parent.requested && parent.localPath.length === 0
                                     && !parent.isDirectory
                            onTriggered: {
                                tick++
                                if (bubbleRoot.node)
                                    bubbleRoot.node.ensureFileAvailable(parent.hash, parent.fileName)
                            }
                        }

                        Rectangle {
                            width: Math.min(parent.width, 320)
                            visible: !fileBlock.isCircle
                            implicitHeight: fileCardRow.implicitHeight + 14
                            radius: 12
                            color: Qt.rgba(0.22, 0.22, 0.22, 0.55)
                            border.color: theme ? theme.border : "#44ffffff"

                            RowLayout {
                                id: fileCardRow
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 8
                                spacing: 10

                                Item {
                                    Layout.preferredWidth: 40
                                    Layout.preferredHeight: 40
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 10
                                        color: Qt.rgba(0.12, 0.12, 0.12, 0.65)
                                    }
                                    NyxIcon {
                                        anchors.centerIn: parent
                                        name: fileBlock.isDirectory ? "folder" : "file"
                                        width: 22
                                        height: 22
                                    }
                                    Rectangle {
                                        width: 14
                                        height: 14
                                        radius: 7
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.margins: -2
                                        color: fileBlock.syncState === 2
                                               ? "#43a047"
                                               : (fileBlock.syncState === 1
                                                  ? "#fb8c00" : "#78909c")
                                        border.color: "#22000000"
                                        NyxIcon {
                                            anchors.centerIn: parent
                                            name: fileBlock.syncState === 2
                                                  ? "check"
                                                  : (fileBlock.syncState === 1
                                                     ? "refresh" : "lock")
                                            width: 8
                                            height: 8
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Label {
                                        Layout.fillWidth: true
                                        text: fileBlock.fileName
                                        color: theme ? theme.textPrimary : "#fff"
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideMiddle
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: fileBlock.isDirectory
                                              ? qsTr("Папка · %1").arg(
                                                    fileBlock.fileSize > 0
                                                    ? (fileBlock.fileSize + qsTr(" файлов"))
                                                    : qsTr("просмотр"))
                                              : (fileBlock.mime + " · "
                                                 + Math.max(1, Math.round(fileBlock.fileSize / 1024))
                                                 + " КБ")
                                        color: theme ? theme.textSecondary : "#aaa"
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: fileBlock.previewText.length > 0
                                        text: fileBlock.previewText
                                        color: theme ? theme.textPrimary : "#fff"
                                        font.family: "monospace"
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 8
                                        elide: Text.ElideRight
                                    }
                                    Image {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: visible ? 140 : 0
                                        visible: fileBlock.isImage
                                                 && fileBlock.localPath.length > 0
                                        source: visible
                                                ? "file:///" + String(fileBlock.localPath)
                                                      .replace(/\\/g, "/") : ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: bubbleRoot.node.openInAppMedia(
                                                fileBlock.localPath, fileBlock.mime,
                                                fileBlock.fileName)
                                        }
                                    }
                                    VideoOutput {
                                        id: fileVideoOutput
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: visible ? 140 : 0
                                        visible: fileBlock.isVideo
                                                 && fileBlock.localPath.length > 0
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: bubbleRoot.node.openInAppMedia(
                                                fileBlock.localPath, fileBlock.mime,
                                                fileBlock.fileName)
                                        }
                                    }
                                    MediaPlayer {
                                        id: filePlayer
                                        source: fileBlock.localPath.length
                                                ? "file:///" + String(fileBlock.localPath)
                                                      .replace(/\\/g, "/") : ""
                                        audioOutput: AudioOutput {}
                                        videoOutput: fileVideoOutput
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: fileBlock.localPath.length > 0
                                                 && (fileBlock.isAudio
                                                     || fileBlock.isVideo)
                                        spacing: 4
                                        MediaPlaybackControls {
                                            Layout.fillWidth: true
                                            theme: bubbleRoot.theme
                                            player: filePlayer
                                            compact: true
                                        }
                                        IconButton {
                                            visible: fileBlock.isVoice
                                            theme: bubbleRoot.theme
                                            name: "folder"
                                            btnSize: 28
                                            flat: true
                                            ToolTip.visible: hovered
                                            ToolTip.text: qsTr("Открыть сохранённые голосовые")
                                            onClicked: bubbleRoot.node.openChatMediaFolder("voice")
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Button {
                                            visible: fileBlock.isDirectory
                                            text: qsTr("В ресурсы")
                                            onClicked: {
                                                if (bubbleRoot.node)
                                                    bubbleRoot.node.openFolderInResources(
                                                        fileBlock.hash,
                                                        fileBlock.folderRoot,
                                                        fileBlock.folderRel)
                                            }
                                        }
                                        Button {
                                            visible: fileBlock.isDirectory
                                            text: qsTr("Скачать")
                                            onClicked: {
                                                if (bubbleRoot.node)
                                                    bubbleRoot.node.downloadRemoteFolder(
                                                        fileBlock.folderRoot,
                                                        fileBlock.folderRel)
                                            }
                                        }
                                        Button {
                                            visible: !fileBlock.isDirectory
                                                     && (fileBlock.localPath.length === 0
                                                         || (!fileBlock.isAudio
                                                             && !fileBlock.isVideo))
                                            text: {
                                                if (fileBlock.localPath.length === 0)
                                                    return qsTr("Скачать")
                                                if (fileBlock.isAudio)
                                                    return qsTr("Слушать")
                                                if (fileBlock.isImage || fileBlock.isVideo)
                                                    return qsTr("Смотреть")
                                                return qsTr("Открыть")
                                            }
                                            onClicked: {
                                                const card = fileBlock
                                                if (!card.localPath.length) {
                                                    card.requested = true
                                                    bubbleRoot.node.ensureFileAvailable(
                                                        card.hash, card.fileName)
                                                } else if (card.isImage) {
                                                    bubbleRoot.node.openInAppMedia(
                                                        card.localPath, card.mime, card.fileName)
                                                } else {
                                                    bubbleRoot.node.openLocalFile(
                                                        card.localPath, card.mime)
                                                }
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                        Label {
                                            visible: fileBlock.requested
                                                     && fileBlock.localPath.length === 0
                                                     && !fileBlock.isDirectory
                                            text: qsTr("Загрузка…")
                                            color: theme ? theme.textSecondary : "#aaa"
                                            font.pixelSize: 11
                                        }
                                    }
                                }
                            }
                        }

                        Column {
                            width: Math.min(parent.width, 220)
                            spacing: 6
                            visible: fileBlock.isCircle

                            Item {
                                id: circleViewport
                                width: parent.width
                                height: width

                                Rectangle {
                                    id: circleSurface
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: "#11151c"
                                    clip: true
                                    layer.enabled: true
                                    layer.effect: MultiEffect {
                                        maskEnabled: true
                                        maskSource: circleMask
                                    }

                                    VideoOutput {
                                        id: circleVideoOutput
                                        anchors.fill: parent
                                        fillMode: VideoOutput.PreserveAspectCrop
                                    }
                                }

                                Rectangle {
                                    id: circleMask
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: "white"
                                    visible: false
                                    layer.enabled: true
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: "transparent"
                                    border.color: theme ? theme.border : "#55ffffff"
                                    border.width: 2
                                }

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 48
                                    height: 48
                                    radius: 24
                                    color: "#aa000000"
                                    visible: fileBlock.localPath.length === 0
                                             || circlePlayer.playbackState
                                                !== MediaPlayer.PlayingState
                                    NyxIcon {
                                        anchors.centerIn: parent
                                        name: fileBlock.localPath.length === 0
                                              ? "video" : "play"
                                        width: 24
                                        height: 24
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: {
                                        if (!fileBlock.localPath.length) {
                                            fileBlock.requested = true
                                            bubbleRoot.node.ensureFileAvailable(
                                                        fileBlock.hash,
                                                        fileBlock.fileName)
                                        } else if (circlePlayer.playbackState
                                                   === MediaPlayer.PlayingState) {
                                            circlePlayer.pause()
                                        } else {
                                            if (circlePlayer.duration > 0
                                                    && circlePlayer.position
                                                       >= circlePlayer.duration)
                                                circlePlayer.position = 0
                                            circlePlayer.play()
                                        }
                                    }
                                }
                            }

                            MediaPlayer {
                                id: circlePlayer
                                source: fileBlock.localPath.length
                                        ? "file:///" + String(fileBlock.localPath)
                                              .replace(/\\/g, "/") : ""
                                videoOutput: circleVideoOutput
                                audioOutput: AudioOutput {}
                            }

                            RowLayout {
                                width: parent.width
                                visible: fileBlock.localPath.length > 0
                                spacing: 4
                                MediaPlaybackControls {
                                    Layout.fillWidth: true
                                    theme: bubbleRoot.theme
                                    player: circlePlayer
                                    compact: true
                                }
                                IconButton {
                                    theme: bubbleRoot.theme
                                    name: "folder"
                                    btnSize: 28
                                    flat: true
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Открыть сохранённые видеокружки")
                                    onClicked: bubbleRoot.node.openChatMediaFolder("circle")
                                }
                            }
                        }
                    }

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
                                source: visible && mediaCol.path.length
                                        ? ("file:///" + String(mediaCol.path).replace(/\\/g, "/"))
                                        : ""
                                width: Math.min(parent.width, 280)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                sourceSize.width: 560
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (bubbleRoot.node && mediaCol.path.length)
                                            bubbleRoot.node.openInAppMedia(
                                                mediaCol.path, "image/jpeg", mediaCol.caption)
                                    }
                                }
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
                                        text: qsTr("Смотреть")
                                        onClicked: bubbleRoot.node.openInAppMedia(
                                            mediaCol.path, "video/mp4", mediaCol.caption)
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
                        onCopyRequested: function(text) {
                            if (bubbleRoot.node)
                                bubbleRoot.node.copyToClipboard(text)
                        }
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

    signal contextRequested(string plainText, real sceneX, real sceneY)
}
