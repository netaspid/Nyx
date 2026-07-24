import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"

/**
 * Call overlay.
 * DM video → fullscreen; Field → compact first, expand for fullscreen.
 * Tool chrome uses fixed dark colors so light theme never washes icons out.
 */
Item {
    id: root
    required property var theme
    required property var node
    property bool narrow: false

    readonly property bool visibleCall: node.callState !== "idle" && node.callState !== "ended"
    readonly property bool isVideoActive: node.callVideo && node.callState === "active"
    readonly property bool preferFullscreen: isVideoActive && (!node.callIsFieldRoom || expanded)
    property bool expanded: false

    readonly property int toolSize: narrow ? 56 : 52
    readonly property int toolIcon: narrow ? 22 : 20
    readonly property color toolBg: "#cc2a3140"
    readonly property color toolBgOn: "#cc1b5e20"
    readonly property color toolBgOff: "#cc4a1520"
    readonly property color hangupBg: "#e53935"
    readonly property color answerBg: "#43a047"

    visible: visibleCall
    z: 900
    anchors.fill: parent

    onVisibleCallChanged: {
        if (!visibleCall) expanded = false
        else if (isVideoActive && !node.callIsFieldRoom) expanded = true
    }
    onIsVideoActiveChanged: {
        if (isVideoActive && !node.callIsFieldRoom) expanded = true
        if (!isVideoActive) expanded = false
    }

    // Telegram-like: icon stays ~40% of circle; AbstractButton must NOT stretch the Image.
    component CallTool: AbstractButton {
        id: btn
        property string iconName: "settings"
        property color bg: root.toolBg
        property int size: root.toolSize
        property int iconPx: root.toolIcon
        property bool solid: false
        implicitWidth: size
        implicitHeight: size
        padding: 0
        hoverEnabled: true
        background: Rectangle {
            radius: width / 2
            color: btn.pressed ? Qt.darker(btn.bg, 1.12) : btn.bg
            border.color: btn.solid ? "transparent" : "#55ffffff"
            border.width: btn.solid ? 0 : 1
        }
        contentItem: Item {
            // Fill button box so layout is stable; icon is centered and small.
            implicitWidth: btn.size
            implicitHeight: btn.size
            NyxIcon {
                anchors.centerIn: parent
                name: btn.iconName
                width: btn.iconPx
                height: btn.iconPx
                sourceSize: Qt.size(btn.iconPx, btn.iconPx)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.preferFullscreen ? "#e0080a0c" : "#99000000"
        visible: root.visibleCall
        // Leave bottom chrome free for taps (Android / SurfaceView edge cases).
        MouseArea {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.bottomMargin: root.narrow ? 200 : 160
            onClicked: {}
        }
    }

    // —— Fullscreen ——
    Item {
        id: stage
        anchors.fill: parent
        visible: root.visibleCall && root.preferFullscreen
        z: 1

        Rectangle { anchors.fill: parent; color: "#0a0c10" }

        Image {
            id: remoteFs
            anchors.fill: parent
            visible: node.callRemoteFrameUrl.toString().length > 0
            fillMode: Image.PreserveAspectCrop
            source: node.callRemoteFrameUrl
            cache: false
            asynchronous: false
        }

        // Name stub when there is no remote video yet / peer camera off (black → still Image).
        Rectangle {
            anchors.fill: parent
            visible: !remoteFs.visible
            color: "#0a0c10"
            Label {
                anchors.centerIn: parent
                text: node.callTitle
                color: "#8b9bab"
                font.pixelSize: Math.round(18 * theme.fontScale)
            }
        }

        Rectangle {
            id: pip
            width: root.narrow ? Math.min(112, parent.width * 0.28) : 144
            height: width * 9 / 16
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: root.narrow ? 16 : 24
            anchors.bottomMargin: controlsCol.height + (root.narrow ? 28 : 36)
            radius: 10
            color: "#1a1e28"
            border.color: "#3d4654"
            border.width: 1
            visible: root.isVideoActive
            clip: true
            z: 3
            Image {
                anchors.fill: parent
                visible: node.callCameraOn && node.callLocalFrameUrl.toString().length > 0
                fillMode: Image.PreserveAspectCrop
                source: node.callLocalFrameUrl
                cache: false
                asynchronous: false
            }
            Label {
                anchors.centerIn: parent
                visible: !node.callCameraOn || node.callLocalFrameUrl.toString().length === 0
                text: qsTr("Вы")
                color: "#8b9bab"
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
        }

        RowLayout {
            id: topBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: root.narrow ? 12 : 20
            spacing: 10
            z: 4

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: node.callIsFieldRoom ? qsTr("В комнате") : qsTr("На линии")
                    color: "#8b9bab"
                    font.pixelSize: 12
                }
                Label {
                    Layout.fillWidth: true
                    text: node.callTitle
                    color: "#ffffff"
                    font.pixelSize: Math.round(16 * theme.fontScale)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
            }

            CallTool {
                visible: node.callIsFieldRoom || !root.narrow
                iconName: "collapse"
                onClicked: root.expanded = false
            }
        }

        Flickable {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: topBar.bottom
            anchors.margins: root.narrow ? 12 : 20
            anchors.topMargin: 8
            height: visible ? 40 : 0
            contentWidth: peerRow.width
            clip: true
            visible: node.callIsFieldRoom
                     && (node.callVideoPeers.length > 1 || node.callRosterPeers.length > 1)
            z: 4
            Row {
                id: peerRow
                spacing: 8
                Repeater {
                    model: node.callVideoPeers.length > 0 ? node.callVideoPeers : node.callRosterPeers
                    delegate: Rectangle {
                        required property var modelData
                        height: 36
                        width: peerLab.implicitWidth + 20
                        radius: 18
                        color: modelData.focused ? "#5288c1" : "#2a3140"
                        Label {
                            id: peerLab
                            anchors.centerIn: parent
                            text: modelData.nickname
                            color: "#ffffff"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: modelData.userId && modelData.userId.length > 0
                            onClicked: node.setCallFocusedPeer(modelData.userId)
                        }
                    }
                }
            }
        }

        ColumnLayout {
            id: controlsCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: root.narrow ? 16 : 24
            spacing: 14
            z: 5

            // Telegram-style: one row of circular icon buttons (hangup is red circle).
            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: root.narrow ? 14 : 16

                CallTool {
                    visible: node.callState === "active"
                    iconName: node.callMicMuted ? "mic-off" : "mic"
                    bg: node.callMicMuted ? root.toolBgOff : root.toolBg
                    onClicked: node.toggleCallMicMuted()
                }
                CallTool {
                    visible: root.isVideoActive
                    iconName: node.callCameraOn ? "video" : "video-off"
                    bg: node.callCameraOn ? root.toolBgOn : root.toolBg
                    onClicked: node.toggleCallCamera()
                }
                CallTool {
                    visible: root.isVideoActive && node.callCameraOn && node.callCanSwitchCamera
                    iconName: "camera-switch"
                    onClicked: node.switchCallCamera()
                }
                CallTool {
                    visible: Qt.platform.os === "android" && node.callState === "active"
                    iconName: node.callSpeakerphone ? "speaker" : "speaker-off"
                    bg: node.callSpeakerphone ? root.toolBgOn : root.toolBg
                    onClicked: node.toggleCallSpeakerphone()
                }
                CallTool {
                    visible: node.callIsFieldRoom
                             && (node.callVideoPeers.length > 0 || node.callRosterPeers.length > 0)
                    iconName: "people"
                    onClicked: root.expanded = true
                }
                CallTool {
                    iconName: "phone"
                    bg: root.hangupBg
                    solid: true
                    size: root.toolSize + 4
                    onClicked: node.hangupCall()
                }
            }
        }
    }

    // —— Compact card ——
    Rectangle {
        anchors.centerIn: parent
        width: root.narrow ? Math.min(parent.width - 24, parent.width) : Math.min(400, parent.width - 48)
        height: Math.min(col.implicitHeight + 40, parent.height - 48)
        radius: 14
        color: theme.bgPanel
        border.color: theme.border
        border.width: 1
        visible: root.visibleCall && !root.preferFullscreen
        z: 2

        ColumnLayout {
            id: col
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: root.narrow ? 16 : 20
            spacing: 14

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    switch (node.callState) {
                    case "outgoing":
                    case "ringing": return qsTr("Вызов…")
                    case "incoming":
                        return node.callIsFieldRoom || node.activeChatKind === 1
                               ? qsTr("Комната в поле") : qsTr("Входящий звонок")
                    case "active":
                        return node.callIsFieldRoom ? qsTr("В комнате") : qsTr("На линии")
                    default: return qsTr("Звонок")
                    }
                }
                color: theme.textPrimary
                font.pixelSize: Math.round(16 * theme.fontScale)
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: node.callTitle
                color: theme.textSecondary
                font.pixelSize: Math.round(14 * theme.fontScale)
                elide: Text.ElideRight
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: root.isVideoActive ? (root.narrow ? 200 : 180) : 0
                visible: root.isVideoActive
                Rectangle {
                    anchors.fill: parent
                    radius: 10
                    color: "#0a0c10"
                    clip: true
                    Image {
                        anchors.fill: parent
                        visible: node.callRemoteFrameUrl.toString().length > 0
                        fillMode: Image.PreserveAspectCrop
                        source: node.callRemoteFrameUrl
                        cache: false
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: node.callRemoteFrameUrl.toString().length === 0
                        text: qsTr("Ожидание видео…")
                        color: theme.textMuted
                        font.pixelSize: 12
                    }
                    Rectangle {
                        width: 72
                        height: width * 9 / 16
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 8
                        radius: 6
                        color: "#1a1e28"
                        visible: root.isVideoActive
                        clip: true
                        Image {
                            anchors.fill: parent
                            visible: node.callCameraOn && node.callLocalFrameUrl.toString().length > 0
                            fillMode: Image.PreserveAspectCrop
                            source: node.callLocalFrameUrl
                            cache: false
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: !node.callCameraOn || node.callLocalFrameUrl.toString().length === 0
                            text: qsTr("Вы")
                            color: theme.textMuted
                            font.pixelSize: 11
                        }
                    }
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: 8
                visible: node.callIsFieldRoom && node.callVideoPeers.length > 1
                Repeater {
                    model: node.callVideoPeers
                    delegate: Rectangle {
                        required property var modelData
                        height: 32
                        width: cLab.implicitWidth + 16
                        radius: 16
                        color: modelData.focused ? theme.accent : theme.btnSecondary
                        Label {
                            id: cLab
                            anchors.centerIn: parent
                            text: modelData.nickname
                            color: theme.textPrimary
                            font.pixelSize: 11
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: node.setCallFocusedPeer(modelData.userId)
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: node.callVideo ? qsTr("Видео") : qsTr("Аудио")
                color: theme.textMuted
                font.pixelSize: 12
                visible: node.callState !== "active" || !node.callVideo
            }

            // Incoming — Telegram: green Answer + red Decline with captions
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: incomingRow.implicitHeight
                visible: node.callState === "incoming"
                Row {
                    id: incomingRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 40
                    Column {
                        spacing: 8
                        width: Math.max(answerBtn.width, answerLab.implicitWidth)
                        CallTool {
                            id: answerBtn
                            anchors.horizontalCenter: parent.horizontalCenter
                            iconName: "phone"
                            bg: root.answerBg
                            solid: true
                            size: root.toolSize + 10
                            iconPx: root.toolIcon + 2
                            onClicked: node.acceptCall()
                        }
                        Label {
                            id: answerLab
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: node.callIsFieldRoom || node.activeChatKind === 1
                                  ? qsTr("Войти") : qsTr("Ответить")
                            color: theme.textPrimary
                            font.pixelSize: 13
                        }
                    }
                    Column {
                        spacing: 8
                        width: Math.max(declineBtn.width, declineLab.implicitWidth)
                        CallTool {
                            id: declineBtn
                            anchors.horizontalCenter: parent.horizontalCenter
                            iconName: "phone"
                            bg: root.hangupBg
                            solid: true
                            size: root.toolSize + 10
                            iconPx: root.toolIcon + 2
                            onClicked: node.rejectCall()
                        }
                        Label {
                            id: declineLab
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: node.callIsFieldRoom || node.activeChatKind === 1
                                  ? qsTr("Позже") : qsTr("Сбросить")
                            color: theme.textPrimary
                            font.pixelSize: 13
                        }
                    }
                }
            }

            // Active / ringing tools — circular row including hangup
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: activeTools.implicitHeight
                visible: node.callState === "outgoing" || node.callState === "ringing"
                         || node.callState === "active"
                Row {
                    id: activeTools
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 12

                CallTool {
                    visible: root.isVideoActive
                    iconName: "expand"
                    size: 44
                    onClicked: root.expanded = true
                }
                CallTool {
                    visible: node.callState === "active"
                    iconName: node.callMicMuted ? "mic-off" : "mic"
                    size: 44
                    bg: node.callMicMuted ? root.toolBgOff : root.toolBg
                    onClicked: node.toggleCallMicMuted()
                }
                CallTool {
                    visible: root.isVideoActive
                    iconName: node.callCameraOn ? "video" : "video-off"
                    size: 44
                    bg: node.callCameraOn ? root.toolBgOn : root.toolBg
                    onClicked: node.toggleCallCamera()
                }
                CallTool {
                    visible: root.isVideoActive && node.callCameraOn && node.callCanSwitchCamera
                    iconName: "camera-switch"
                    size: 44
                    onClicked: node.switchCallCamera()
                }
                CallTool {
                    visible: Qt.platform.os === "android" && node.callState === "active"
                    iconName: node.callSpeakerphone ? "speaker" : "speaker-off"
                    size: 44
                    bg: node.callSpeakerphone ? root.toolBgOn : root.toolBg
                    onClicked: node.toggleCallSpeakerphone()
                }
                CallTool {
                    iconName: "phone"
                    bg: root.hangupBg
                    solid: true
                    size: 48
                    onClicked: node.hangupCall()
                }
                }
            }
        }
    }
}
