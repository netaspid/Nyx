import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"

/**
 * Полноэкранный UX звонка: входящий / исходящий / активный.
 * Видео всегда на весь экран; свёртки в маленькую карточку нет.
 */
Item {
    id: root
    required property var theme
    required property var node
    property bool narrow: false

    readonly property bool visibleCall: node.callState !== "idle" && node.callState !== "ended"
    readonly property bool isVideoActive: node.callVideo && node.callState === "active"
    readonly property bool isRinging: node.callState === "incoming"
                                      || node.callState === "outgoing"
                                      || node.callState === "ringing"

    visible: visibleCall
    z: 900
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: "#f0080a0c"
        visible: root.visibleCall
        MouseArea { anchors.fill: parent; onClicked: {} }
    }

    // Remote / placeholder stage
    Image {
        id: remoteFs
        anchors.fill: parent
        visible: root.isVideoActive && node.callRemoteFrameUrl.toString().length > 0
        fillMode: Image.PreserveAspectCrop
        source: node.callRemoteFrameUrl
        cache: false
        asynchronous: false
    }

    Rectangle {
        anchors.fill: parent
        visible: root.visibleCall && !remoteFs.visible
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#12151c" }
            GradientStop { position: 1.0; color: "#0a0c10" }
        }
        Column {
            anchors.centerIn: parent
            spacing: 10
            width: parent.width - 48
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: {
                    switch (node.callState) {
                    case "outgoing":
                    case "ringing": return qsTr("Вызов…")
                    case "incoming":
                        return node.callIsFieldRoom ? qsTr("Комната в поле") : qsTr("Входящий звонок")
                    case "active":
                        return node.callIsFieldRoom ? qsTr("В комнате") : qsTr("На линии")
                    default: return qsTr("Звонок")
                    }
                }
                color: theme.textMuted
                font.pixelSize: 14
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: node.callTitle
                color: theme.textPrimary
                font.pixelSize: Math.round(28 * theme.fontScale)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: root.isVideoActive
                text: qsTr("Ожидание видео…")
                color: theme.textMuted
                font.pixelSize: 13
            }
        }
    }

    // Top bar
    RowLayout {
        id: topBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: root.narrow ? 16 : 24
        anchors.topMargin: root.narrow ? 20 : 28
        spacing: 10
        z: 4
        visible: root.visibleCall

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                Layout.fillWidth: true
                text: node.callVideo ? qsTr("Видеозвонок") : qsTr("Аудиозвонок")
                color: theme.textMuted
                font.pixelSize: 12
            }
            Label {
                Layout.fillWidth: true
                text: node.callTitle
                color: theme.textPrimary
                font.pixelSize: Math.round(18 * theme.fontScale)
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
    }

    // Field peer strip
    Flickable {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.margins: root.narrow ? 12 : 20
        height: visible ? 40 : 0
        contentWidth: peerRow.width
        clip: true
        visible: root.isVideoActive && node.callIsFieldRoom && node.callVideoPeers.length > 1
        z: 4
        flickableDirection: Flickable.HorizontalFlick

        Row {
            id: peerRow
            spacing: 8
            Repeater {
                model: node.callVideoPeers
                delegate: Rectangle {
                    required property var modelData
                    height: 36
                    width: peerLab.implicitWidth + 20
                    radius: 18
                    color: modelData.focused ? theme.accent : theme.btnSecondary
                    Label {
                        id: peerLab
                        anchors.centerIn: parent
                        text: modelData.nickname
                        color: theme.textPrimary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: node.setCallFocusedPeer(modelData.userId)
                    }
                }
            }
        }
    }

    // Local PiP
    Rectangle {
        width: root.narrow ? Math.min(120, parent.width * 0.3) : 168
        height: width * 3 / 4
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: root.narrow ? 16 : 24
        anchors.bottomMargin: controlsCol.height + (root.narrow ? 28 : 36)
        radius: 12
        color: "#1a1e28"
        border.color: theme.border
        border.width: 1
        visible: root.isVideoActive && node.callLocalFrameUrl.toString().length > 0
        clip: true
        z: 5

        Image {
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            source: node.callLocalFrameUrl
            cache: false
            asynchronous: false
        }
    }

    // Bottom controls
    ColumnLayout {
        id: controlsCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: root.narrow ? 20 : 28
        spacing: 14
        z: 6
        visible: root.visibleCall

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 16
            visible: node.callState === "incoming"

            NyxButtonSecondary {
                Layout.preferredHeight: root.narrow ? 52 : 48
                Layout.preferredWidth: root.narrow ? 130 : 140
                theme: root.theme
                text: node.callIsFieldRoom ? qsTr("Позже") : qsTr("Отклонить")
                onClicked: node.rejectCall()
            }
            NyxButton {
                Layout.preferredHeight: root.narrow ? 52 : 48
                Layout.preferredWidth: root.narrow ? 130 : 140
                theme: root.theme
                text: node.callIsFieldRoom ? qsTr("Войти") : qsTr("Принять")
                onClicked: node.acceptCall()
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 18
            visible: node.callState === "outgoing" || node.callState === "ringing"
                     || node.callState === "active"

            IconButton {
                visible: root.isVideoActive && node.callCanSwitchCamera
                theme: root.theme
                name: "camera-switch"
                btnSize: root.narrow ? 52 : 48
                onClicked: node.switchCallCamera()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Камера")
            }

            NyxButton {
                Layout.preferredHeight: root.narrow ? 52 : 48
                Layout.preferredWidth: root.narrow ? 180 : 160
                theme: root.theme
                text: node.callIsFieldRoom ? qsTr("Выйти") : qsTr("Сбросить")
                onClicked: node.hangupCall()
            }
        }
    }
}
