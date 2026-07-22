import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../controls"
import "../js/MarkdownFormat.js" as Md

Item {
    id: root
    required property var theme
    required property var node
    required property var avatarColorFn
    required property var formatMsgTimeFn

    property bool composerPreview: false
    /** Облачко форматирования видно (в потоке layout, не Overlay). */
    property bool markdownToolsOpen: false
    /** Не сбрасывать тулбар/превью при клике по кнопкам композера (Aa, эмодзи…). */
    property bool composerToolsSticky: false
    property string composerChatKey: ""


    function openChatMeta() {
        if (node.activeChatKind === 1)
            node.openFieldInfo(node.activeChatRefId)
        else
            node.openPeerInfo(node.activeChatRefId)
    }

    readonly property string activeFieldSubtitle: {
        if (node.activeChatKind !== 1) return qsTr("эфир открыт")
        for (let i = 0; i < node.groupList.length; ++i) {
            const g = node.groupList[i]
            if (String(g.groupId).toLowerCase() === String(node.activeChatRefId).toLowerCase())
                return qsTr("эфир открыт · %1 участн.").arg(g.memberCount || 0)
        }
        return qsTr("эфир открыт")
    }

    readonly property string headerStatusText: {
        if (!node.peerTitle.length) return ""
        const st = node.sessionStateForKey(node.activeChatKey)
        if (st === "connecting") return qsTr("переподключение…")
        if (node.inChat) {
            if (node.activeChatKind === 1) return root.activeFieldSubtitle
            return node.peerConnectionLabel.length ? node.peerConnectionLabel : qsTr("на связи")
        }
        return node.peerStatusText
    }

    function bumpMarkdownIdle() {
        if (!msgField.activeFocus && !root.composerToolsSticky) {
            root.hideMarkdownToolbar()
            return
        }
        mdIdleTimer.restart()
        if (!node.canSendMessage) {
            root.hideMarkdownToolbar()
            return
        }
        if (msgField.text.length === 0 && msgField.selectedText.length === 0) {
            root.hideMarkdownToolbar()
            return
        }
        root.markdownToolsOpen = true
    }

    function hideMarkdownToolbar() {
        root.markdownToolsOpen = false
    }

    /** Дефолтный ввод: без Aa-превью и без облачка форматирования. */
    function resetComposerTools() {
        composerResetTimer.stop()
        root.composerToolsSticky = false
        root.composerPreview = false
        mdIdleTimer.stop()
        root.hideMarkdownToolbar()
        if (mentionPopup.opened)
            mentionPopup.close()
    }

    function scheduleComposerReset() {
        composerResetTimer.restart()
    }

    function holdComposerToolsBriefly() {
        root.composerToolsSticky = true
        composerStickyTimer.restart()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 56
        color: theme.bgChatHeader

        RowLayout {
            anchors.fill: parent
            anchors.margins: theme.spacing
            spacing: 12

            AvatarBadge {
                visible: node.peerTitle.length > 0
                size: 40
                label: node.peerTitle
                baseColor: avatarColorFn(node.peerTitle)
                textColor: theme.textPrimary
                imageSource: node.activeChatKind === 0
                             ? node.peerAvatarPath(node.activeChatRefId)
                             : ""
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Item {
                    Layout.fillWidth: true
                    implicitHeight: fieldTitleLabel.implicitHeight
                    Label {
                        id: fieldTitleLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        text: node.peerTitle.length ? node.peerTitle : qsTr("Выберите чат")
                        color: node.activeChatKind === 1 ? theme.accent : theme.textPrimary
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: node.peerTitle.length > 0
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        ToolTip.visible: containsMouse
                        ToolTip.text: node.activeChatKind === 1
                                      ? qsTr("Участники и invite поля")
                                      : qsTr("Профиль собеседника")
                        onClicked: root.openChatMeta()
                    }
                }
                Item {
                    Layout.fillWidth: true
                    implicitHeight: Math.max(16, headerStatusLabel.implicitHeight)
                    visible: node.peerTitle.length > 0
                    Label {
                        id: headerStatusLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        text: root.headerStatusText
                        color: {
                            const st = node.sessionStateForKey(node.activeChatKey)
                            if (st === "connecting") return theme.accent
                            if (node.inChat) return theme.online
                            return theme.textSecondary
                        }
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        z: 2
                        acceptedButtons: Qt.LeftButton
                        enabled: node.peerTitle.length > 0
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        ToolTip.visible: containsMouse
                        ToolTip.text: node.activeChatKind === 1
                                      ? qsTr("Информация о поле и участники")
                                      : qsTr("Профиль собеседника")
                        onClicked: function(mouse) {
                            mouse.accepted = true
                            root.openChatMeta()
                        }
                    }
                }
            }

            IconButton {
                visible: node.activeChatKind === 1 && node.peerTitle.length > 0
                theme: root.theme
                glyph: "\uE716"
                ToolTip.text: qsTr("Участники поля")
                onClicked: root.openChatMeta()
            }

            NyxTextField {
                id: msgSearch
                Layout.preferredWidth: 140
                visible: node.peerTitle.length > 0 && node.messages.count > 0
                theme: root.theme
                placeholderText: qsTr("Поиск")
                onTextChanged: node.searchMessages(text)
            }

            IconButton {
                visible: node.peerTitle.length > 0 && node.canStartCall
                theme: root.theme
                glyph: "\uE717"
                ToolTip.text: node.activeChatKind === 1 ? qsTr("Открыть аудиокомнату")
                                                       : qsTr("Аудиозвонок")
                onClicked: node.startCall(false)
            }
            IconButton {
                visible: node.peerTitle.length > 0 && node.canStartCall
                theme: root.theme
                glyph: "\uE714"
                ToolTip.text: node.activeChatKind === 1 ? qsTr("Открыть видеокомнату")
                                                       : qsTr("Видеозвонок")
                onClicked: node.startCall(true)
            }

            IconButton {
                visible: node.inChat || node.sessionStateForKey(node.activeChatKey) === "live"
                         || node.sessionStateForKey(node.activeChatKey) === "connecting"
                theme: root.theme
                glyph: "\uE711"
                ToolTip.text: qsTr("Отключиться")
                onClicked: node.disconnectChat(node.activeChatKey)
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.border
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        EmptyState {
            anchors.centerIn: parent
            width: parent.width * 0.7
            visible: !node.peerTitle.length && !node.inChat
            theme: root.theme
            emoji: "💬"
            title: qsTr("Выберите чат")
            hint: qsTr("Чаты слева · Друзья · Поля · или «+ Связь»")
        }

        ListView {
            id: msgList
            anchors.fill: parent
            anchors.margins: theme.chatSideMargin
            visible: node.peerTitle.length > 0 || node.inChat
            clip: true
            spacing: 6
            model: node.messages
            delegate: ChatBubble {
                width: msgList.width
                listWidth: msgList.width
                theme: root.theme
                node: root.node
            }
            onCountChanged: Qt.callLater(function() { msgList.positionViewAtEnd() })
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        visible: node.peerTitle.length > 0 || node.inChat

        ProgressBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 4
            visible: node.fileProgressVisible
            from: 0
            to: 100
            value: node.fileProgressPercent
        }

        Rectangle {
            Layout.fillWidth: true
            color: theme.bgInputBar
            implicitHeight: composerCol.implicitHeight + theme.spacing + 4

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: theme.border
            }

            ColumnLayout {
                id: composerCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: theme.spacing
                anchors.rightMargin: theme.spacing
                spacing: 6

                // В потоке над полем — не перекрывает превью/текст
                MarkdownToolbar {
                    id: mdToolbar
                    Layout.alignment: Qt.AlignLeft
                    Layout.preferredHeight: root.markdownToolsOpen ? implicitHeight : 0
                    theme: root.theme
                    visible: root.markdownToolsOpen
                    clip: true
                    onActivity: {
                        root.holdComposerToolsBriefly()
                        msgField.forceActiveFocus()
                        root.bumpMarkdownIdle()
                    }
                    onFormat: function(action) {
                        root.holdComposerToolsBriefly()
                        msgField.forceActiveFocus()
                        root.applyFormat(action)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                // Единая оболочка: в превью — сверху «так увидят», снизу ввод
                Rectangle {
                    id: composerShell
                    Layout.fillWidth: true
                    Layout.preferredHeight: {
                        const editorH = Math.min(200, Math.max(44, msgField.contentHeight + 20))
                        if (!root.composerPreview)
                            return editorH + 12
                        const bubbleH = Math.min(
                            160,
                            Math.max(36, composerPreviewBody.implicitHeight + 16 + 18))
                        const previewH = 14 + bubbleH // подпись + облачко
                        return Math.min(420, 12 + previewH + 9 + editorH)
                    }
                    radius: 18
                    color: theme.inputBg
                    border.color: msgField.activeFocus || root.composerPreview
                                  ? theme.focusRing : theme.border
                    border.width: (msgField.activeFocus || root.composerPreview) ? 2 : 1
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 0

                        // —— верх: превью по размеру текста ——
                        ColumnLayout {
                            id: previewSection
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.composerPreview
                                ? (14 + Math.min(
                                    160,
                                    Math.max(36, composerPreviewBody.implicitHeight + 16 + 18)))
                                : 0
                            spacing: 2
                            visible: root.composerPreview
                            clip: true

                            Label {
                                text: qsTr("Так увидят")
                                color: theme.textMuted
                                font.pixelSize: 10
                                Layout.leftMargin: 8
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.min(
                                    160,
                                    Math.max(36, composerPreviewBody.implicitHeight + 16 + 18))
                                radius: 12
                                color: theme.bubbleOut
                                border.color: theme.border
                                border.width: 1
                                clip: true

                                Flickable {
                                    id: previewFlick
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    contentWidth: width
                                    contentHeight: composerPreviewBody.implicitHeight
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    interactive: contentHeight > height

                                    ComposerPreview {
                                        id: composerPreviewBody
                                        width: previewFlick.width
                                        theme: root.theme
                                        sourceText: msgField.text
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.topMargin: 4
                            Layout.bottomMargin: 4
                            visible: root.composerPreview
                            color: theme.border
                        }

                        // —— низ: редактор ——
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 32
                            spacing: 2

                            IconButton {
                                theme: root.theme
                                glyph: "\uE16C"
                                btnSize: 36
                                enabled: node.inChat || node.sessionUnlocked
                                ToolTip.text: qsTr("Файлы")
                                onPressed: root.holdComposerToolsBriefly()
                                onClicked: node.openFilesView()
                            }

                            IconButton {
                                id: emojiBtn
                                theme: root.theme
                                glyph: "\uE76E"
                                btnSize: 36
                                enabled: node.canSendMessage
                                ToolTip.text: qsTr("Смайлики")
                                onPressed: root.holdComposerToolsBriefly()
                                onClicked: {
                                    root.holdComposerToolsBriefly()
                                    const p = emojiBtn.mapToItem(Overlay.overlay, 0, 0)
                                    emojiPicker.x = Math.min(Math.max(8, p.x),
                                                             Overlay.overlay.width - emojiPicker.width - 8)
                                    emojiPicker.y = Math.max(8, p.y - emojiPicker.height - 8)
                                    emojiPicker.open()
                                }
                            }

                            NyxButtonSecondary {
                                theme: root.theme
                                text: qsTr("Aa")
                                enabled: node.canSendMessage
                                implicitWidth: 48
                                implicitHeight: 32
                                ToolTip.text: root.composerPreview
                                              ? qsTr("Скрыть превью")
                                              : qsTr("Превью: сверху «как увидят», снизу текст")
                                onPressed: root.holdComposerToolsBriefly()
                                onClicked: {
                                    root.composerPreview = !root.composerPreview
                                    msgField.forceActiveFocus()
                                    root.bumpMarkdownIdle()
                                }
                                background: Rectangle {
                                    radius: theme.radiusBtn
                                    color: {
                                        if (!parent.enabled) return theme.inputBg
                                        if (root.composerPreview) return theme.accent
                                        if (parent.hovered) return theme.btnSecondaryHover
                                        return theme.btnSecondary
                                    }
                                    border.color: theme.border
                                    border.width: root.composerPreview ? 0 : 1
                                }
                                contentItem: Text {
                                    text: parent.text
                                    color: root.composerPreview ? "#ffffff" : theme.textPrimary
                                    font.pixelSize: Math.round(13 * theme.fontScale)
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            TextArea {
                                id: msgField
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                wrapMode: TextEdit.Wrap
                                verticalAlignment: root.composerPreview
                                                   ? TextEdit.AlignTop
                                                   : TextEdit.AlignVCenter
                                enabled: node.canSendMessage
                                color: theme.textPrimary
                                placeholderText: node.canSendMessage
                                    ? qsTr("Сообщение · /me · @ник · Ctrl+Enter")
                                    : (node.activeChatKind === 1
                                       ? qsTr("Эфир закрыт — нельзя писать")
                                       : qsTr("Нет связи — нельзя писать"))
                                placeholderTextColor: theme.textMuted
                                font.pixelSize: 14
                                font.family: "Segoe UI"
                                selectByMouse: true
                                leftPadding: 6
                                rightPadding: 4
                                topPadding: 8
                                bottomPadding: 8
                                background: Item {}
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        composerResetTimer.stop()
                                        root.bumpMarkdownIdle()
                                    } else {
                                        root.scheduleComposerReset()
                                    }
                                }
                                onTextChanged: {
                                    if (activeFocus)
                                        root.bumpMarkdownIdle()
                                    root.updateMentionPopup()
                                }
                                onSelectedTextChanged: {
                                    if (activeFocus)
                                        root.bumpMarkdownIdle()
                                }
                                onCursorPositionChanged: root.updateMentionPopup()
                                Keys.onPressed: function(event) {
                                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                            && (event.modifiers & Qt.ControlModifier)) {
                                        sendMsg()
                                        event.accepted = true
                                        return
                                    }
                                    const ctrl = event.modifiers & Qt.ControlModifier
                                    const shift = event.modifiers & Qt.ShiftModifier
                                    if (ctrl && !shift && event.key === Qt.Key_B) {
                                        applyFormat("bold"); event.accepted = true
                                    } else if (ctrl && !shift && event.key === Qt.Key_I) {
                                        applyFormat("italic"); event.accepted = true
                                    } else if (ctrl && !shift && event.key === Qt.Key_U) {
                                        applyFormat("underline"); event.accepted = true
                                    } else if (ctrl && shift && event.key === Qt.Key_X) {
                                        applyFormat("strike"); event.accepted = true
                                    } else if (ctrl && shift && event.key === Qt.Key_P) {
                                        applyFormat("spoiler"); event.accepted = true
                                    } else if (ctrl && !shift && event.key === Qt.Key_K) {
                                        applyFormat("link"); event.accepted = true
                                    } else if (ctrl && !shift && event.key === Qt.Key_1) {
                                        applyFormat("h1"); event.accepted = true
                                    } else if (ctrl && !shift && event.key === Qt.Key_2) {
                                        applyFormat("h2"); event.accepted = true
                                    } else if (ctrl && !shift && event.key === Qt.Key_3) {
                                        applyFormat("h3"); event.accepted = true
                                    } else if (ctrl && shift && event.key === Qt.Key_L) {
                                        applyFormat("ul"); event.accepted = true
                                    } else if (ctrl && shift && event.key === Qt.Key_M) {
                                        applyFormat("formula"); event.accepted = true
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.alignment: Qt.AlignBottom
                    width: 44
                    height: 44
                    radius: 22
                    color: (node.canSendMessage && msgField.text.trim().length > 0)
                          ? (sendMouse.pressed ? theme.accentPress : theme.accent)
                          : theme.btnSecondary
                    border.color: theme.border
                    border.width: (node.canSendMessage && msgField.text.trim().length > 0) ? 0 : 1
                    Text {
                        anchors.centerIn: parent
                        text: "\uE724"
                        font.family: "Segoe MDL2 Assets"
                        font.pixelSize: 18
                        color: (node.canSendMessage && msgField.text.trim().length > 0)
                               ? "#ffffff"
                               : theme.textMuted
                    }
                    MouseArea {
                        id: sendMouse
                        anchors.fill: parent
                        enabled: node.canSendMessage && msgField.text.trim().length > 0
                        onClicked: sendMsg()
                    }
                }
                } // RowLayout shell+send
            } // ColumnLayout composerCol
        }
    }

    } // ColumnLayout

    Timer {
        id: mdIdleTimer
        interval: 10000
        repeat: false
        onTriggered: root.hideMarkdownToolbar()
    }

    Timer {
        id: composerResetTimer
        interval: 100
        repeat: false
        onTriggered: {
            if (msgField.activeFocus || root.composerToolsSticky)
                return
            if (linkDialog.visible || codeLangDialog.visible)
                return
            if (typeof emojiPicker !== "undefined" && emojiPicker.opened)
                return
            if (mentionPopup.opened)
                return
            root.resetComposerTools()
        }
    }

    Timer {
        id: composerStickyTimer
        interval: 400
        repeat: false
        onTriggered: {
            root.composerToolsSticky = false
            if (!msgField.activeFocus)
                root.scheduleComposerReset()
        }
    }

    Connections {
        target: node
        function onChatChanged() {
            // chatChanged шумный — сбрасываем только при смене чата
            const key = node.activeChatKey
            if (key === root.composerChatKey)
                return
            root.composerChatKey = key
            root.resetComposerTools()
        }
    }

    MentionPopup {
        id: mentionPopup
        parent: Overlay.overlay
        theme: root.theme
        onPicked: function(nickname, userId) {
            root.insertMention(nickname, userId)
        }
    }

    Dialog {
        id: linkDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Ссылка")
        standardButtons: Dialog.NoButton
        width: 320
        padding: theme.spacing
        background: Rectangle {
            color: theme.bgSidebar
            radius: theme.radiusBtn
            border.color: theme.border
        }
        ColumnLayout {
            width: parent ? parent.width : 280
            NyxTextField {
                id: linkUrlField
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: "https://"
            }
        }
        footer: Item {
            implicitHeight: linkFooter.implicitHeight + theme.spacing
            width: parent ? parent.width : 320
            RowLayout {
                id: linkFooter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: theme.spacing
                spacing: 8
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Отмена")
                    onClicked: linkDialog.reject()
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("OK")
                    onClicked: linkDialog.accept()
                }
            }
        }
        onAccepted: {
            const url = linkUrlField.text.trim()
            if (!url.length) return
            const label = root.linkText.length ? root.linkText : url
            const start = msgField.selectionStart
            const end = msgField.selectionEnd
            if (start !== end)
                msgField.remove(start, end)
            msgField.insert(msgField.cursorPosition, "[" + label + "](" + url + ")")
            msgField.forceActiveFocus()
            root.bumpMarkdownIdle()
        }
        onClosed: {
            if (!msgField.activeFocus)
                root.scheduleComposerReset()
        }
    }

    Dialog {
        id: codeLangDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Язык кода")
        standardButtons: Dialog.NoButton
        width: 340
        padding: theme.spacing
        background: Rectangle {
            color: theme.bgSidebar
            radius: theme.radiusBtn
            border.color: theme.border
        }
        ColumnLayout {
            width: parent ? parent.width : 300
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Подсветка/метка синтаксиса (cpp, python, js, qml…). Можно оставить пустым.")
                color: theme.textSecondary
                font.pixelSize: 12
            }
            NyxTextField {
                id: codeLangField
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("например cpp")
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: ["cpp", "python", "js", "qml", "json", "bash", "html", "css"]
                    NyxButtonSecondary {
                        required property string modelData
                        theme: root.theme
                        text: modelData
                        implicitHeight: 28
                        onClicked: codeLangField.text = modelData
                    }
                }
            }
        }
        footer: Item {
            implicitHeight: codeLangFooter.implicitHeight + theme.spacing
            width: parent ? parent.width : 340
            RowLayout {
                id: codeLangFooter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: theme.spacing
                spacing: 8
                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Отмена")
                    onClicked: codeLangDialog.reject()
                }
                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("OK")
                    onClicked: codeLangDialog.accept()
                }
            }
        }
        onAccepted: root.insertFenceWithLang(codeLangField.text)
        onClosed: {
            if (!msgField.activeFocus)
                root.scheduleComposerReset()
        }
    }

    EmojiPicker {
        id: emojiPicker
        theme: root.theme
        onEmojiChosen: function(emoji) {
            root.insertAtCursor(emoji)
            msgField.forceActiveFocus()
            root.bumpMarkdownIdle()
        }
        onClosed: {
            if (!msgField.activeFocus)
                root.scheduleComposerReset()
        }
    }

    function wrapSelection(marker) {
        wrapSelectionCustom(marker, marker)
    }

    function wrapSelectionCustom(left, right) {
        const start = msgField.selectionStart
        const end = msgField.selectionEnd
        const t = msgField.text
        if (start !== end) {
            const selected = t.substring(start, end)
            msgField.remove(start, end)
            msgField.insert(start, left + selected + right)
            msgField.select(start + left.length, start + left.length + selected.length)
        } else {
            msgField.insert(msgField.cursorPosition, left + right)
            msgField.cursorPosition = msgField.cursorPosition - right.length
        }
        msgField.forceActiveFocus()
        root.bumpMarkdownIdle()
    }

    function applyQuote() {
        const start = msgField.selectionStart
        const end = msgField.selectionEnd
        const t = msgField.text
        if (start !== end) {
            const selected = t.substring(start, end)
            const quoted = selected.split("\n").map(function(line) {
                return line.startsWith("> ") ? line : ("> " + line)
            }).join("\n")
            msgField.remove(start, end)
            msgField.insert(start, quoted)
        } else {
            const pos = msgField.cursorPosition
            const lineStart = t.lastIndexOf("\n", Math.max(0, pos - 1)) + 1
            msgField.insert(lineStart, "> ")
            msgField.cursorPosition = pos + 2
        }
        msgField.forceActiveFocus()
        root.bumpMarkdownIdle()
    }

    property int fenceSelStart: 0
    property int fenceSelEnd: 0
    property string fenceSelected: ""

    function applyFence() {
        root.holdComposerToolsBriefly()
        root.fenceSelStart = msgField.selectionStart
        root.fenceSelEnd = msgField.selectionEnd
        root.fenceSelected = msgField.selectedText
        codeLangField.text = ""
        codeLangDialog.open()
    }

    function insertFenceWithLang(lang) {
        const tag = String(lang || "").trim()
        const open = tag.length ? ("```" + tag + "\n") : "```\n"
        const start = root.fenceSelStart
        const end = root.fenceSelEnd
        if (start !== end) {
            msgField.remove(start, end)
            msgField.insert(start, open + root.fenceSelected + "\n```")
        } else {
            msgField.insert(msgField.cursorPosition, open + "\n```")
            msgField.cursorPosition = msgField.cursorPosition - 4
        }
        msgField.forceActiveFocus()
        root.bumpMarkdownIdle()
    }

    function applyLinePrefix(prefix) {
        const t = msgField.text
        const pos = msgField.cursorPosition
        const lineStart = t.lastIndexOf("\n", Math.max(0, pos - 1)) + 1
        msgField.insert(lineStart, prefix)
        msgField.forceActiveFocus()
        root.bumpMarkdownIdle()
    }

    function applyFormat(action) {
        if (action === "bold") wrapSelection("**")
        else if (action === "italic") wrapSelection("_")
        else if (action === "underline") wrapSelection("__")
        else if (action === "strike") wrapSelection("~~")
        else if (action === "spoiler") wrapSelection("||")
        else if (action === "code") wrapSelection("`")
        else if (action === "fence") applyFence()
        else if (action === "quote") applyQuote()
        else if (action === "h1") applyLinePrefix("# ")
        else if (action === "h2") applyLinePrefix("## ")
        else if (action === "h3") applyLinePrefix("### ")
        else if (action === "ul") applyLinePrefix("- ")
        else if (action === "ol") applyLinePrefix("1. ")
        else if (action === "table") {
            insertAtCursor("| | |\n| --- | --- |\n| | |\n")
            msgField.forceActiveFocus()
            root.bumpMarkdownIdle()
        } else if (action === "formula") {
            wrapSelectionCustom("$$", "$$")
        } else if (action === "media") {
            const md = node.pickChatMediaMarkdown()
            if (md && md.length) {
                if (msgField.text.length && !msgField.text.endsWith("\n"))
                    insertAtCursor("\n")
                insertAtCursor(md + "\n")
                msgField.forceActiveFocus()
                root.bumpMarkdownIdle()
            }
        } else if (action === "link") {
            root.holdComposerToolsBriefly()
            root.linkText = msgField.selectedText
            linkUrlField.text = "https://"
            linkDialog.open()
        }
    }

    property string linkText: ""
    property int mentionStart: -1

    function mentionCandidates() {
        const out = []
        if (node.activeChatKind === 1) {
            for (let i = 0; i < node.groupList.length; ++i) {
                const g = node.groupList[i]
                if (String(g.groupId).toLowerCase() !== String(node.activeChatRefId).toLowerCase())
                    continue
                const members = g.members || []
                for (let j = 0; j < members.length; ++j)
                    out.push(members[j])
            }
        } else if (node.activeChatKind === 0 && node.activeChatRefId.length) {
            const c = node.contactInfo(node.activeChatRefId)
            if (c && c.userId)
                out.push(c)
        }
        return out
    }

    function updateMentionPopup() {
        const t = msgField.text
        const pos = msgField.cursorPosition
        const before = t.substring(0, pos)
        const at = before.lastIndexOf("@")
        if (at < 0 || (at > 0 && /[^\s]/.test(before.charAt(at - 1)))) {
            if (mentionPopup.opened) mentionPopup.close()
            root.mentionStart = -1
            return
        }
        const query = before.substring(at + 1)
        if (query.indexOf(" ") >= 0 || query.indexOf("\n") >= 0) {
            if (mentionPopup.opened) mentionPopup.close()
            root.mentionStart = -1
            return
        }
        const q = query.toLowerCase()
        const all = root.mentionCandidates()
        const filtered = []
        for (let i = 0; i < all.length; ++i) {
            const m = all[i]
            const nick = String(m.nickname || "").toLowerCase()
            const shortId = String(m.idShort || "").toLowerCase()
            if (!q.length || nick.indexOf(q) === 0 || shortId.indexOf(q) === 0)
                filtered.push(m)
        }
        if (!filtered.length) {
            if (mentionPopup.opened) mentionPopup.close()
            return
        }
        root.mentionStart = at
        mentionPopup.candidates = filtered
        mentionPopup.placeAbove(msgField)
        if (!mentionPopup.opened) mentionPopup.open()
    }

    function insertMention(nickname, userId) {
        if (root.mentionStart < 0) return
        const pos = msgField.cursorPosition
        msgField.remove(root.mentionStart, pos)
        msgField.insert(root.mentionStart, "[@" + nickname + "](nyx-user:" + userId + ") ")
        root.mentionStart = -1
        if (mentionPopup.opened) mentionPopup.close()
        msgField.forceActiveFocus()
        root.bumpMarkdownIdle()
    }

    function insertAtCursor(text) {
        msgField.insert(msgField.cursorPosition, text)
    }

    function sendMsg() {
        if (!node.canSendMessage || msgField.text.trim().length === 0) return
        node.sendMessage(msgField.text)
        msgField.text = ""
        root.composerPreview = false
        root.hideMarkdownToolbar()
        if (mentionPopup.opened) mentionPopup.close()
    }
}
