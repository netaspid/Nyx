import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes

/** Бренд Nyx: созвездие + подпись «Nyx» (единый lockup, чистый QML). */
Item {
    id: root

    property var theme
    property bool compact: false
    property bool large: false

    readonly property bool dark: theme ? theme.darkMode : true
    readonly property real scale: large ? 1.12 : 1.0
    readonly property color starHi: dark ? "#f8fafc" : "#ffffff"
    readonly property color starLo: dark ? "#93c5fd" : "#5288c1"
    readonly property color lineColor: dark ? "#5288c1" : "#6ab2f2"
    readonly property color wordFill: dark ? "#eef2ff" : "#0f172a"
    readonly property color wordOutline: dark ? "#6366f1" : "#818cf8"
    readonly property color wordGlow: dark ? "#5288c1" : "#93c5fd"

    implicitWidth: row.implicitWidth
    implicitHeight: Math.max(34, wordmark.implicitHeight) * scale

    RowLayout {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: compact ? 0 : Math.round(8 * scale)

        Item {
            id: constellation
            Layout.preferredWidth: Math.round(26 * scale)
            Layout.preferredHeight: Math.round(30 * scale)
            Layout.alignment: Qt.AlignVCenter

            transform: Scale {
                origin.x: constellation.width / 2
                origin.y: constellation.height / 2
                xScale: root.scale
                yScale: root.scale
            }

            Shape {
                width: 26
                height: 30
                antialiasing: true

                ShapePath {
                    strokeColor: root.lineColor
                    strokeWidth: 1.2
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    startX: 6
                    startY: 22
                    PathLine { x: 13; y: 6 }
                    PathLine { x: 22; y: 17 }
                }
            }

            Repeater {
                model: [
                    { cx: 6, cy: 22, r: 2.2, core: starLo, glow: 0.2 },
                    { cx: 13, cy: 6, r: 3.0, core: starHi, glow: 0.35 },
                    { cx: 22, cy: 17, r: 1.8, core: starLo, glow: 0.15 }
                ]

                delegate: Item {
                    x: modelData.cx - modelData.r - 2
                    y: modelData.cy - modelData.r - 2
                    width: (modelData.r + 2) * 2
                    height: (modelData.r + 2) * 2

                    Rectangle {
                        anchors.centerIn: parent
                        width: modelData.r * 2 + 5
                        height: modelData.r * 2 + 5
                        radius: width / 2
                        color: root.lineColor
                        opacity: modelData.glow
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: modelData.r * 2
                        height: modelData.r * 2
                        radius: modelData.r
                        color: modelData.core
                    }
                }
            }
        }

        Item {
            id: wordmark
            visible: !compact
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: nameText.width
            implicitHeight: nameText.height

            transform: Scale {
                origin.x: 0
                origin.y: wordmark.height / 2
                xScale: root.scale
                yScale: root.scale
            }

            Text {
                text: "Nyx"
                font.pixelSize: 26
                font.weight: Font.Bold
                font.letterSpacing: 1.8
                font.family: "Segoe UI"
                color: root.wordGlow
                opacity: 0.4
                x: 1
                y: 1
            }

            Text {
                id: nameText
                text: "Nyx"
                font.pixelSize: 26
                font.weight: Font.Bold
                font.letterSpacing: 1.8
                font.family: "Segoe UI"
                color: root.wordFill
                style: Text.Outline
                styleColor: root.wordOutline
            }
        }
    }
}
