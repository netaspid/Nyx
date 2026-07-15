import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Компактный чекбокс в стиле Nyx. */
Item {
    id: root
    required property var theme
    property alias text: label.text
    property bool checked: false

    signal toggled(bool checked)
    signal clicked()

    implicitWidth: row.implicitWidth
    implicitHeight: Math.max(18, row.implicitHeight)

    opacity: root.enabled ? 1 : 0.45

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Rectangle {
            id: box
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            Layout.alignment: Qt.AlignVCenter
            radius: 4
            color: root.checked ? theme.accent : theme.inputBg
            border.width: 1
            border.color: root.checked ? theme.accent : theme.border

            Text {
                anchors.centerIn: parent
                visible: root.checked
                text: "\u2713"
                color: theme.textPrimary
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
        }

        Label {
            id: label
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            color: theme.textSecondary
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            root.checked = !root.checked
            root.toggled(root.checked)
            root.clicked()
        }
    }
}
