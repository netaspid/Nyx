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
    title: node.inChat ? node.peerTitle : (node.profileNickname + " — Nyx")
    color: theme.bgApp

    Theme { id: theme }

    palette.window: theme.bgApp
    palette.windowText: theme.textPrimary
    palette.base: theme.inputBg
    palette.text: theme.textPrimary
    palette.highlight: theme.accent
    palette.highlightedText: theme.textPrimary
    palette.placeholderText: theme.textMuted

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

    onActiveChanged: node.setWindowActive(active)

    onClosing: function(close) {
        if (node.trayAvailable) {
            close.accepted = false
            root.hide()
            node.hideToTray()
        }
    }

    Shortcut {
        sequences: ["Esc"]
        onActivated: {
            if (node.connectionPanelOpen)
                node.connectionPanelOpen = false
            else if (node.inChat)
                node.disconnectSession()
        }
    }

    Shortcut {
        sequences: ["Ctrl+K"]
        onActivated: node.connectionPanelOpen = true
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ChatListPanel {
            Layout.preferredWidth: root.width < 860 ? (root.width * 0.42) : 320
            Layout.fillHeight: true
            theme: theme
            node: node
            avatarColorFn: avatarColor
            visible: root.width >= 720
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: theme.border
            visible: root.width >= 720
        }

        ChatView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: theme
            node: node
            avatarColorFn: avatarColor
            formatMsgTimeFn: formatMsgTime
        }
    }

    ConnectionDrawer {
        theme: theme
        node: node
        avatarColorFn: avatarColor
    }

    OnboardingDialog {
        theme: theme
        node: node
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        visible: node.toast.length > 0
        radius: 20
        color: theme.toastBg
        Label {
            anchors.centerIn: parent
            anchors.margins: 16
            text: node.toast
            color: theme.textPrimary
        }
        Timer {
            interval: 1800
            running: parent.visible
            onTriggered: node.clearToast()
        }
    }

    Connections {
        target: node
        function onChatChanged() {
            if (node.inChat) node.refreshChatList()
        }
        function onIncomingMessage(author, preview) {
            if (!root.active)
                root.requestAttention(Qt.InformationalRequest)
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
