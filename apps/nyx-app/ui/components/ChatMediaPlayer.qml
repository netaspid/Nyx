import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "."

Item {
    id: root
    required property var theme
    required property var node
    z: 950
    anchors.fill: parent
    visible: node && node.inAppMediaOpen
    focus: visible

    readonly property string path: node ? (node.inAppMediaPath || "") : ""
    readonly property string mime: node ? (node.inAppMediaMime || "") : ""
    readonly property string title: node ? (node.inAppMediaTitle || "") : ""
    readonly property bool isImage: mime.indexOf("image/") === 0
    readonly property bool isAudio: mime.indexOf("audio/") === 0
    readonly property bool isVideo: mime.indexOf("video/") === 0
    readonly property string sourceUrl: path.length
                                        ? ("file:///" + String(path).replace(/\\/g, "/"))
                                        : ""

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            node.closeInAppMedia()
            event.accepted = true
        } else if (event.key === Qt.Key_Space && (isAudio || isVideo)) {
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause()
            else
                player.play()
            event.accepted = true
        }
    }

    onVisibleChanged: {
        if (visible) {
            forceActiveFocus()
            if (isAudio || isVideo) {
                player.source = sourceUrl
            }
        } else {
            player.stop()
            player.source = ""
        }
    }

    onSourceUrlChanged: {
        if (!visible) return
        if (isAudio || isVideo) {
            player.source = sourceUrl
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#e0080a0c"
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.node.closeInAppMedia()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: root.title.length ? root.title : qsTr("Медиа")
                color: "#ffffff"
                font.pixelSize: 16
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
            }
            IconButton {
                theme: root.theme
                name: "close"
                ToolTip.text: qsTr("Закрыть")
                onClicked: root.node.closeInAppMedia()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Image {
                anchors.fill: parent
                visible: root.isImage && root.sourceUrl.length > 0
                source: visible ? root.sourceUrl : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                MouseArea {
                    anchors.fill: parent
                    onClicked: {  }
                }
            }

            VideoOutput {
                id: videoOut
                anchors.fill: parent
                visible: root.isVideo
                fillMode: VideoOutput.PreserveAspectFit
            }

            Rectangle {
                anchors.centerIn: parent
                visible: root.isAudio
                width: Math.min(parent.width - 24, 360)
                height: 120
                radius: 16
                color: "#cc2a3140"
                border.color: "#55ffffff"
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10
                    NyxIcon {
                        Layout.alignment: Qt.AlignHCenter
                        name: "mic"
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: root.title.length ? root.title : qsTr("Аудиосообщение")
                        color: "#ffffff"
                        elide: Text.ElideMiddle
                    }
                }
            }
        }

        MediaPlaybackControls {
            Layout.fillWidth: true
            visible: root.isAudio || root.isVideo
            theme: root.theme
            player: player
            compact: width < 360
        }
    }

    MediaPlayer {
        id: player
        videoOutput: videoOut
        audioOutput: AudioOutput {}
    }
}
