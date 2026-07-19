import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Нижняя строка статуса: сессии, диагностика, advanced connect. */
Rectangle {
    id: root
    required property var theme
    property var node: null
    property string text: ""
    property bool busy: false

    implicitHeight: 30
    color: theme.bgApp

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: theme.spacing
        anchors.rightMargin: theme.spacing
        anchors.topMargin: 1
        spacing: 8

        Rectangle {
            Layout.preferredWidth: 8
            Layout.preferredHeight: 8
            radius: 4
            color: busy ? theme.accent : theme.online
            visible: text.length > 0 || (node && node.sessionSummary)
        }

        Label {
            Layout.fillWidth: true
            // Важно: не читать свой text внутри binding — иначе строка раздувается.
            text: {
                const summary = node ? node.sessionSummary : ""
                const status = root.text
                if (status.length > 0 && summary.length > 0)
                    return summary + " · " + status
                if (summary.length > 0)
                    return summary
                return status.length > 0 ? status : qsTr("Готов")
            }
            color: theme.textSecondary
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }

        ToolButton {
            visible: node !== null
            text: qsTr("Сеть")
            font.pixelSize: 11
            implicitHeight: 22
            onClicked: if (node) node.connectionPanelOpen = !node.connectionPanelOpen
            contentItem: Label {
                text: parent.text
                color: theme.textMuted
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 4
                color: parent.hovered ? theme.btnSecondaryHover : "transparent"
                border.color: theme.border
            }
        }

        ToolButton {
            visible: node !== null && node.inChat
            text: qsTr("Откл.")
            font.pixelSize: 11
            implicitHeight: 22
            onClicked: if (node) node.disconnectChat(node.activeChatKey)
            contentItem: Label {
                text: parent.text
                color: theme.textMuted
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 4
                color: parent.hovered ? theme.btnSecondaryHover : "transparent"
                border.color: theme.border
            }
        }
    }
}
