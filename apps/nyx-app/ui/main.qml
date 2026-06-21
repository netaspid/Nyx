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
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: app.sessionUnlocked
         ? (app.inChat ? app.peerTitle : (app.profileNickname + " — Nyx"))
         : "Nyx"
    color: appTheme.bgApp

    Theme { id: appTheme }

    palette.window: appTheme.bgApp
    palette.windowText: appTheme.textPrimary
    palette.base: appTheme.inputBg
    palette.text: appTheme.textPrimary
    palette.highlight: appTheme.accent
    palette.highlightedText: appTheme.textPrimary
    palette.placeholderText: appTheme.textMuted

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

    onActiveChanged: app.windowActive = active

    onClosing: function(close) {
        close.accepted = true
    }

    Shortcut {
        sequences: ["Esc"]
        onActivated: {
            if (app.connectionPanelOpen)
                app.connectionPanelOpen = false
            else if (app.inChat)
                app.disconnectSession()
        }
    }

    Shortcut {
        sequences: ["Ctrl+K"]
        onActivated: app.connectionPanelOpen = !app.connectionPanelOpen
    }

    AccountGate {
        theme: appTheme
        node: app
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        visible: app.sessionUnlocked

        ChatListPanel {
            Layout.preferredWidth: root.width < 860 ? (root.width * 0.42) : 320
            Layout.fillHeight: true
            theme: appTheme
            node: app
            avatarColorFn: avatarColor
            visible: root.width >= 720
            onSettingsRequested: settingsDialog.open()
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: appTheme.border
            visible: root.width >= 720
        }

        ChatView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: appTheme
            node: app
            avatarColorFn: avatarColor
            formatMsgTimeFn: formatMsgTime
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

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        visible: app.toast.length > 0
        radius: 20
        color: appTheme.toastBg
        Label {
            anchors.centerIn: parent
            anchors.margins: 16
            text: app.toast
            color: appTheme.textPrimary
        }
        Timer {
            interval: 1800
            running: parent.visible
            onTriggered: app.clearToast()
        }
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
