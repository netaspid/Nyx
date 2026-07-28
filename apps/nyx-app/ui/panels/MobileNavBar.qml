import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Rectangle {
    id: root
    required property var theme
    required property var node

    implicitHeight: 56
    color: theme.bgSidebar

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: theme.border
    }

    function goList(mode) {
        node.sidebarMode = mode
        node.showChatView()
        if (node.inChat)
            node.leaveChat()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 2
        anchors.rightMargin: 2
        spacing: 0

        component NavCell: AbstractButton {
            id: cell
            property string iconName
            property string label
            property bool active: false
            property bool accentBtn: false

            Layout.fillWidth: true
            Layout.fillHeight: true
            hoverEnabled: Qt.platform.os !== "android"

            background: Rectangle {
                color: {
                    if (cell.pressed)
                        return root.theme.btnSecondaryHover
                    if (cell.hovered && Qt.platform.os !== "android")
                        return root.theme.btnSecondaryHover
                    return "transparent"
                }
            }

            contentItem: ColumnLayout {
                spacing: 2
                Item {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: {
                            if (cell.accentBtn)
                                return root.theme.accent
                            if (cell.active)
                                return root.theme.accent
                            return "transparent"
                        }
                    }
                    NyxIcon {
                        anchors.centerIn: parent
                        name: cell.iconName
                        width: 16
                        height: 16
                    }
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: cell.label
                    color: cell.active || cell.accentBtn ? root.theme.accent : root.theme.textMuted
                    font.pixelSize: 9
                }
            }
        }

        NavCell {
            iconName: "chat"
            label: qsTr("Чаты")
            active: node.sidebarMode === 0 && node.mainViewMode !== 1
            onClicked: root.goList(0)
        }
        NavCell {
            iconName: "people"
            label: qsTr("Друзья")
            active: node.sidebarMode === 1 && node.mainViewMode !== 1
            onClicked: root.goList(1)
        }
        NavCell {
            iconName: "field"
            label: qsTr("Поля")
            active: node.sidebarMode === 2 && node.mainViewMode !== 1
            onClicked: root.goList(2)
        }
        NavCell {
            iconName: "folder"
            label: qsTr("Файлы")
            active: node.mainViewMode === 1
            onClicked: node.openFilesView()
        }
        NavCell {
            iconName: "link"
            label: qsTr("Связь")
            accentBtn: true
            active: !!node.connectionPanelOpen
            onClicked: node.connectionPanelOpen = true
        }
    }
}
