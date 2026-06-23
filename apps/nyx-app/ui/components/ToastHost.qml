import QtQuick
import QtQuick.Controls

/** Всплывающее уведомление внизу справа. */
Rectangle {
    id: root
    required property var theme
    property string message: ""
    property bool isError: false

    visible: message.length > 0

    z: 60
    radius: theme.radiusBtn
    color: isError ? theme.toastErrorBg : theme.toastBg
    border.color: isError ? "#e57373" : theme.border
    border.width: 1
    opacity: visible ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    implicitWidth: msgLabel.implicitWidth + 28
    implicitHeight: msgLabel.implicitHeight + 16

    Row {
        anchors.centerIn: parent
        spacing: 8
        padding: 8

        Text {
            visible: isError
            text: "\uE783"
            font.family: "Segoe MDL2 Assets"
            font.pixelSize: 14
            color: "#ffcdd2"
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            visible: !isError
            text: "\uE73E"
            font.family: "Segoe MDL2 Assets"
            font.pixelSize: 14
            color: theme.accent
            anchors.verticalCenter: parent.verticalCenter
        }

        Label {
            id: msgLabel
            text: message
            color: theme.textPrimary
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            width: Math.min(320, implicitWidth)
        }
    }

    Timer {
        id: hideTimer
        interval: isError ? 4200 : 2600
        running: root.visible
        onTriggered: if (typeof clearFn === "function") clearFn()
    }

    property var clearFn: null
}
