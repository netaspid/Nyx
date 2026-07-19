import QtQuick

/** Спойлер в духе Telegram: точки поверх цвета пузыря, клик — раскрыть. */
Item {
    id: root
    property string body: ""
    property bool revealed: false
    property color bubbleColor: "#2b5278"
    property color textColor: "#ffffff"
    property real maxWidth: 280

    signal revealRequested()

    readonly property color veilColor: Qt.rgba(
        Math.min(1, bubbleColor.r * 0.82 + 0.08),
        Math.min(1, bubbleColor.g * 0.82 + 0.08),
        Math.min(1, bubbleColor.b * 0.82 + 0.08),
        1)

    implicitWidth: revealed
                   ? Math.min(maxWidth, Math.max(12, bodyText.implicitWidth))
                   : Math.min(maxWidth, Math.max(36, Math.min(body.length * 6.5 + 16, maxWidth)))
    implicitHeight: revealed ? bodyText.implicitHeight : Math.max(18, bodyText.font.pixelSize + 4)

    Text {
        id: bodyText
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(root.maxWidth, implicitWidth)
        text: root.body
        color: root.textColor
        font.pixelSize: 15
        font.family: "Segoe UI"
        wrapMode: Text.Wrap
        opacity: root.revealed ? 1 : 0
        visible: root.revealed
    }

    // Скрытое измерение ширины
    Text {
        id: measure
        visible: false
        text: root.body
        font.pixelSize: 15
        font.family: "Segoe UI"
    }

    Rectangle {
        id: veil
        anchors.fill: parent
        visible: !root.revealed
        radius: 4
        color: root.veilColor
        clip: true

        Canvas {
            id: dots
            anchors.fill: parent
            property real phase: 0
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                if (w < 2 || h < 2) return
                // Светлые точки как в Telegram
                const r = root.textColor.r
                const g = root.textColor.g
                const b = root.textColor.b
                const n = Math.max(12, Math.floor(w * h / 28))
                for (let i = 0; i < n; ++i) {
                    const seed = i * 17.13 + phase * 40
                    const x = ((Math.sin(seed) * 0.5 + 0.5) * w)
                    const y = ((Math.cos(seed * 1.3) * 0.5 + 0.5) * h)
                    const a = 0.25 + 0.55 * (0.5 + 0.5 * Math.sin(seed + phase * 6))
                    const rad = 0.8 + (i % 3) * 0.35
                    ctx.beginPath()
                    ctx.fillStyle = "rgba(" + Math.round(r * 255) + "," + Math.round(g * 255)
                                    + "," + Math.round(b * 255) + "," + a.toFixed(3) + ")"
                    ctx.arc(x, y, rad, 0, Math.PI * 2)
                    ctx.fill()
                }
            }
        }

        Timer {
            interval: 48
            running: veil.visible
            repeat: true
            onTriggered: {
                dots.phase += 0.045
                dots.requestPaint()
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.revealRequested()
        }
    }

    Component.onCompleted: {
        if (!revealed)
            implicitWidth = Math.min(maxWidth, Math.max(36, measure.implicitWidth))
    }
}
