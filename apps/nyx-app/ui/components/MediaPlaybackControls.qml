import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "../controls"
import "."

ColumnLayout {
    id: root
    required property var theme
    required property var player
    property bool compact: false
    spacing: 4

    readonly property bool playing:
        player && player.playbackState === MediaPlayer.PlayingState

    function formatTime(ms) {
        const seconds = Math.max(0, Math.floor(ms / 1000))
        const minutes = Math.floor(seconds / 60)
        const rest = seconds % 60
        return minutes + ":" + (rest < 10 ? "0" : "") + rest
    }

    function togglePlayback() {
        if (!player)
            return
        if (playing) {
            player.pause()
        } else {
            if (player.duration > 0 && player.position >= player.duration)
                player.position = 0
            player.play()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        IconButton {
            theme: root.theme
            name: root.playing ? "pause" : "play"
            btnSize: root.compact ? 34 : 40
            ToolTip.text: root.playing ? qsTr("Пауза") : qsTr("Воспроизвести")
            onClicked: root.togglePlayback()
        }

        Slider {
            id: positionSlider
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, root.player ? root.player.duration : 0)
            value: root.player ? root.player.position : 0
            live: false
            onMoved: {
                if (root.player)
                    root.player.position = value
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Label {
            text: root.formatTime(root.player ? root.player.position : 0)
                  + " / "
                  + root.formatTime(root.player ? root.player.duration : 0)
            color: root.theme.textSecondary
            font.pixelSize: 11
        }

        Item {
            Layout.fillWidth: true
        }

        Label {
            visible: !root.compact
            text: qsTr("Скорость")
            color: root.theme.textSecondary
            font.pixelSize: 11
        }

        NyxComboBox {
            id: speedBox
            theme: root.theme
            Layout.preferredWidth: root.compact ? 70 : 78
            implicitHeight: 30
            model: ["0.5×", "0.75×", "1×", "1.25×", "1.5×", "2×"]
            currentIndex: 2
            onActivated: {
                const rates = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]
                if (root.player)
                    root.player.playbackRate = rates[currentIndex]
            }
        }
    }
}
