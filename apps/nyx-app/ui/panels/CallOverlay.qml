import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

/** Оверлей входящего/активного звонка. */
Item {
    id: root
    required property var theme
    required property var node

    readonly property bool visibleCall: node.callState !== "idle" && node.callState !== "ended"
    visible: visibleCall
    z: 900
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: "#99000000"
        visible: root.visibleCall

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 48)
        height: col.implicitHeight + 40
        radius: 14
        color: theme.bgPanel
        border.color: theme.border
        border.width: 1
        visible: root.visibleCall

        ColumnLayout {
            id: col
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 20
            spacing: 14

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    switch (node.callState) {
                    case "outgoing": return qsTr("Вызов…")
                    case "ringing": return qsTr("Вызов…")
                    case "incoming":
                        return node.callIsFieldRoom || node.activeChatKind === 1
                               ? qsTr("Комната в поле")
                               : qsTr("Входящий звонок")
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

            Image {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 280
                Layout.preferredHeight: 160
                visible: node.callVideo && node.callState === "active"
                         && node.callRemoteFrameUrl.toString().length > 0
                fillMode: Image.PreserveAspectFit
                source: node.callRemoteFrameUrl
                cache: false
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: node.callVideo ? qsTr("Видео") : qsTr("Аудио")
                color: theme.textMuted
                font.pixelSize: 12
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                NyxButton {
                    visible: node.callState === "incoming"
                    theme: root.theme
                    text: node.activeChatKind === 1 ? qsTr("Войти") : qsTr("Принять")
                    onClicked: node.acceptCall()
                }
                NyxButtonSecondary {
                    visible: node.callState === "incoming"
                    theme: root.theme
                    text: node.activeChatKind === 1 ? qsTr("Позже") : qsTr("Отклонить")
                    onClicked: node.rejectCall()
                }
                NyxButtonSecondary {
                    visible: node.callState === "outgoing" || node.callState === "ringing"
                             || node.callState === "active"
                    theme: root.theme
                    text: node.callIsFieldRoom ? qsTr("Выйти") : qsTr("Сбросить")
                    onClicked: node.hangupCall()
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                visible: node.callState === "active"
                text: qsTr("Аудио на линии")
                color: theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }
}
