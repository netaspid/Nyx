import QtQuick

/** Design tokens — Telegram dark / light. */
QtObject {
    id: root

    property bool darkMode: true

    function toggleDarkMode() {
        darkMode = !darkMode
    }

    function setDarkMode(value) {
        darkMode = value
    }

    readonly property color bgApp: darkMode ? "#17212b" : "#ffffff"
    readonly property color bgSidebar: darkMode ? "#0e1621" : "#f0f2f5"
    readonly property color bgChat: darkMode ? "#0e1621" : "#e4ddd4"
    readonly property color bgChatHeader: darkMode ? "#17212b" : "#ffffff"
    readonly property color bgInputBar: darkMode ? "#17212b" : "#f0f2f5"
    readonly property color bubbleIn: darkMode ? "#2a3949" : "#ffffff"
    readonly property color bubbleOut: darkMode ? "#2b5278" : "#effdde"
    readonly property color bubbleTextIn: darkMode ? "#ffffff" : "#000000"
    readonly property color bubbleTextOut: darkMode ? "#ffffff" : "#000000"
    readonly property color accent: darkMode ? "#5288c1" : "#3390ec"
    readonly property color accentHover: darkMode ? "#6a9fd4" : "#4da3f7"
    readonly property color accentPress: darkMode ? "#4674a8" : "#2b7fd4"
    readonly property color btnSecondary: darkMode ? "#242f3d" : "#ffffff"
    readonly property color btnSecondaryHover: darkMode ? "#2f3d4d" : "#f5f5f5"
    readonly property color textPrimary: darkMode ? "#ffffff" : "#000000"
    readonly property color textSecondary: darkMode ? "#8b9bab" : "#707579"
    readonly property color textMuted: darkMode ? "#5d6d7e" : "#999999"
    readonly property color inputBg: darkMode ? "#242f3d" : "#ffffff"
    readonly property color border: darkMode ? "#2b3847" : "#dadce0"
    readonly property color online: darkMode ? "#4caf50" : "#4caf50"
    /** Подсветка неактивного чата/поля в списке. */
    readonly property color offlineRow: darkMode ? "#1a222c" : "#eceff1"
    readonly property color offlineBadge: darkMode ? "#6b4e4e" : "#e57373"
    readonly property color toastBg: darkMode ? "#243040" : "#ffffff"
    readonly property color toastErrorBg: darkMode ? "#3a2428" : "#fff5f5"
    readonly property color focusRing: darkMode ? "#6ab2f2" : "#3390ec"
    readonly property real fontScale: 1.0
    readonly property int radiusBubble: 16
    readonly property int radiusBubbleTail: 4
    readonly property int radiusBtn: 8
    readonly property int radiusInput: 10
    readonly property int spacing: 12
    readonly property int bubblePadH: 12
    readonly property int bubblePadV: 10
    readonly property int chatSideMargin: 14
}
