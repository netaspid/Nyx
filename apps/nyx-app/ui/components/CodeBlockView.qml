import QtQuick
import QtQuick.Controls
import "../js/MarkdownFormat.js" as Md

/** Блок кода: тёмная панель, перенос строк, вертикальный скролл для длинных полотен. */
Rectangle {
    id: root
    property string lang: ""
    property string code: ""
    property real maxContentHeight: 280
    signal copyRequested(string text)

    color: "#1a2332"
    radius: 8
    border.color: Qt.rgba(1, 1, 1, 0.08)
    border.width: 1
    clip: true

    // Только колонка + поля; высоту кода не дублировать (раньше был ×2 → пустой хвост)
    implicitHeight: col.implicitHeight + 16

    Column {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 6

        Item {
            width: parent.width
            height: 26

            Label {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.lang === "copy" ? qsTr("Текст")
                      : (root.lang.length ? root.lang : qsTr("Код"))
                color: "#8b9bab"
                font.pixelSize: 11
                font.family: "Segoe UI"
            }

            Rectangle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: copyLabel.implicitWidth + 18
                height: 26
                radius: 7
                color: copyMouse.containsMouse ? "#35445a" : "#29374a"

                Label {
                    id: copyLabel
                    anchors.centerIn: parent
                    text: qsTr("Копировать")
                    color: "#d4d4d4"
                    font.pixelSize: 11
                }
                MouseArea {
                    id: copyMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.copyRequested(root.code)
                }
            }
        }

        Flickable {
            id: flick
            width: parent.width
            height: Math.min(root.maxContentHeight, Math.max(1, codeText.implicitHeight))
            contentWidth: width
            contentHeight: codeText.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height + 1

            Text {
                id: codeText
                width: flick.width
                wrapMode: Text.WrapAnywhere
                textFormat: Text.RichText
                text: Md.highlightCodeWrapped(root.lang, root.code, 56)
                font.family: "Consolas"
                font.pixelSize: 13
                // lineHeight на RichText раздувает implicitHeight без отрисовки — не использовать
                color: "#d4d4d4"
            }

            ScrollBar.vertical: ScrollBar {
                policy: flick.contentHeight > flick.height + 1
                        ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }
        }
    }
}
