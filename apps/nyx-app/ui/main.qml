import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."
import "components"
import "panels"
import "dialogs"

ApplicationWindow {
    id: root
    width: 1100
    height: 740
    minimumWidth: Qt.platform.os === "android" ? 0 : 720
    minimumHeight: Qt.platform.os === "android" ? 0 : 560
    visible: true
    title: app.sessionUnlocked
         ? (app.mainViewMode === 1 ? qsTr("Файлы — Nyx")
            : app.inChat ? app.peerTitle : (app.profileNickname + " — Nyx"))
         : "Nyx"
    color: appTheme.bgApp

    Theme { id: appTheme }

    readonly property bool narrow: width < 720 || Qt.platform.os === "android"
    readonly property bool showChatList: {
        if (!narrow) return true
        if (app.mainViewMode === 1) return false
        return !app.inChat
    }
    readonly property bool showMainContent: {
        if (!narrow) return true
        if (app.mainViewMode === 1) return true
        return app.inChat
    }

    palette.window: appTheme.bgApp
    palette.windowText: appTheme.textPrimary
    palette.base: appTheme.inputBg
    palette.text: appTheme.textPrimary
    palette.highlight: appTheme.accent
    palette.highlightedText: appTheme.textPrimary
    palette.placeholderText: appTheme.textMuted

    Component.onCompleted: {
        if (contentItem) {
            contentItem.forceActiveFocus()
            contentItem.Keys.released.connect(function(event) {
                if (event.key === Qt.Key_Back || event.key === Qt.Key_Backspace) {
                    if (root.handleBack())
                        event.accepted = true
                }
            })
        }
        app.setNativeChromeDark(appTheme.darkMode)
    }
    Connections {
        target: appTheme
        function onDarkModeChanged() { app.setNativeChromeDark(appTheme.darkMode) }
    }

    function formatMsgTime(ms) {
        if (!ms) return ""
        return Qt.formatTime(new Date(ms), "HH:mm")
    }

    function avatarColor(name) {
        var palette = ["#5288c1", "#6ab2f2", "#7b68ee", "#5b9a8b", "#c27856", "#9b59b6"]
        var hash = 0
        for (var i = 0; i < name.length; ++i)
            hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0
        return palette[Math.abs(hash) % palette.length]
    }

    function handleBack() {
        if (app.mainViewMode === 1) {
            app.showChatView()
            return true
        }
        if (app.connectionPanelOpen) {
            app.connectionPanelOpen = false
            return true
        }
        if (root.narrow && app.inChat) {
            app.leaveChat()
            return true
        }
        return false
    }

    onActiveChanged: app.windowActive = active

    onClosing: function(close) {
        close.accepted = true
    }

    Shortcut {
        sequences: ["Esc"]
        onActivated: root.handleBack()
    }

    Shortcut {
        sequences: ["Ctrl+K"]
        onActivated: app.connectionPanelOpen = !app.connectionPanelOpen
    }

    AccountGate {
        theme: appTheme
        node: app
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: app.sessionUnlocked

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ChatListPanel {
                Layout.preferredWidth: root.narrow ? root.width : (root.width < 860 ? (root.width * 0.42) : 320)
                Layout.fillWidth: root.narrow
                Layout.fillHeight: true
                theme: appTheme
                node: app
                avatarColorFn: avatarColor
                visible: root.showChatList
                onSettingsRequested: settingsDialog.open()
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: appTheme.border
                visible: !root.narrow && root.showChatList && root.showMainContent
            }

            MainContentPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                theme: appTheme
                node: app
                avatarColorFn: avatarColor
                formatMsgTimeFn: formatMsgTime
                visible: root.showMainContent
            }
        }

        StatusBar {
            id: statusBar
            Layout.fillWidth: true
            theme: appTheme
            node: app
            text: app.statusText
            busy: app.busy
        }
    }

    ConnectionDrawer {
        theme: appTheme
        node: app
        avatarColorFn: avatarColor
    }

    SettingsDialog {
        id: settingsDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        theme: appTheme
        node: app
    }

    GroupsDialog {
        parent: Overlay.overlay
        anchors.centerIn: parent
        theme: appTheme
        node: app
    }

    FieldInfoDialog {
        parent: Overlay.overlay
        anchors.centerIn: parent
        z: 20
        theme: appTheme
        node: app
        avatarColorFn: avatarColor
    }

    PeerInfoDialog {
        parent: Overlay.overlay
        anchors.centerIn: parent
        z: 30
        theme: appTheme
        node: app
        avatarColorFn: avatarColor
    }

    ToastHost {
        id: toastHost
        parent: Overlay.overlay
        z: 1000
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: root.narrow ? 12 : 20
        anchors.bottomMargin: root.narrow ? 12 : 20
        theme: appTheme
        message: app.toast
        isError: app.toastIsError
        clearFn: function() { app.clearToast() }
    }

    CallOverlay {
        parent: Overlay.overlay
        anchors.fill: parent
        theme: appTheme
        node: app
        narrow: root.narrow
    }

    Connections {
        target: app
        function onChatChanged() {
            if (app.inChat) app.refreshChatList()
        }
        function onIncomingMessage(author, preview) {
            if (!root.active)
                root.alert(0)
        }
        function onShowMainWindow() {
            root.show()
            root.raise()
            root.requestActivate()
        }
        function onRequestCloseToTray() {
            root.hide()
        }
    }
}
