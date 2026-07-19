import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Компактное облачко форматирования в потоке layout (не Overlay). */
Item {
    id: root
    required property var theme

    signal format(string action)
    signal activity()

    implicitWidth: cloud.implicitWidth
    implicitHeight: cloud.implicitHeight
    opacity: 1

    Rectangle {
        id: cloud
        implicitWidth: Math.min(contentRow.implicitWidth + 10, 340)
        implicitHeight: 36
        width: implicitWidth
        height: implicitHeight
        radius: 18
        color: Qt.rgba(theme.toastBg.r, theme.toastBg.g, theme.toastBg.b, 0.94)
        border.color: theme.border
        border.width: 1

        Flickable {
            anchors.fill: parent
            anchors.margins: 5
            clip: true
            contentWidth: contentRow.implicitWidth
            contentHeight: contentRow.implicitHeight
            interactive: contentWidth > width
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: contentRow
                spacing: 2

                Repeater {
                    model: [
                        { label: "H1", tip: qsTr("Заголовок 1"), action: "h1" },
                        { label: "H2", tip: qsTr("Заголовок 2"), action: "h2" },
                        { label: "H3", tip: qsTr("Заголовок 3"), action: "h3" },
                        { label: "B", tip: qsTr("Жирный"), action: "bold" },
                        { label: "I", tip: qsTr("Курсив"), action: "italic" },
                        { label: "U", tip: qsTr("Подчёркнутый"), action: "underline" },
                        { label: "S", tip: qsTr("Зачёркнутый"), action: "strike" },
                        { label: "||", tip: qsTr("Спойлер"), action: "spoiler" },
                        { label: "`", tip: qsTr("Код"), action: "code" },
                        { label: "{ }", tip: qsTr("Блок кода"), action: "fence" },
                        { label: ">", tip: qsTr("Цитата"), action: "quote" },
                        { label: "•", tip: qsTr("Список"), action: "ul" },
                        { label: "1.", tip: qsTr("Нумерованный"), action: "ol" },
                        { label: "табл", tip: qsTr("Таблица"), action: "table" },
                        { label: "∑", tip: qsTr("Формула"), action: "formula" },
                        { label: "мед", tip: qsTr("Фото/видео"), action: "media" },
                        { label: "ссыл", tip: qsTr("Ссылка"), action: "link" }
                    ]
                    Rectangle {
                        required property var modelData
                        width: Math.max(26, tipLabel.implicitWidth + 8)
                        height: 26
                        radius: 8
                        color: ma.containsMouse ? theme.btnSecondaryHover : "transparent"
                        Label {
                            id: tipLabel
                            anchors.centerIn: parent
                            text: modelData.label
                            color: theme.textPrimary
                            font.pixelSize: modelData.label.length > 2 ? 10 : 12
                            font.bold: modelData.label === "B"
                            font.italic: modelData.label === "I"
                            font.underline: modelData.label === "U"
                        }
                        MouseArea {
                            id: ma
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            // Не забирать фокус у TextArea — иначе тулбар «прыгает»/сбрасывается
                            preventStealing: true
                            ToolTip.visible: containsMouse
                            ToolTip.text: modelData.tip
                            onPressed: root.activity()
                            onClicked: root.format(modelData.action)
                        }
                    }
                }
            }
        }
    }
}
