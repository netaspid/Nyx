import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

/** Строка файла или папки в браузере. */
Rectangle {
    id: root
    required property var theme
    required property string name
    required property string hash
    required property string sizeLabel
    required property string mime
    required property bool isRemote
    required property bool isDirectory
    required property string navPath
    required property string rootPath
    property var node

    default property alias actions: actionsLayout.data

    width: ListView.view ? ListView.view.width : parent.width
    height: row.implicitHeight + 16
    radius: theme.radiusBtn
    color: mouseArea.containsMouse ? theme.btnSecondaryHover : theme.btnSecondary
    border.color: theme.border
    border.width: 1

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        Text {
            text: fileGlyph(name, mime, isDirectory)
            font.family: "Segoe MDL2 Assets"
            font.pixelSize: 20
            color: theme.accent
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: name
                color: theme.textPrimary
                font.pixelSize: 13
                font.weight: isDirectory ? Font.DemiBold : Font.Normal
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            Label {
                text: isDirectory ? sizeLabel : (sizeLabel + " · " + mime)
                color: theme.textMuted
                font.pixelSize: 10
            }
        }

        RowLayout {
            id: actionsLayout
            spacing: 6
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: {
            if (isDirectory && node)
                node.browseIntoFolder(navPath)
        }
        onDoubleClicked: {
            if (isDirectory && node)
                node.browseIntoFolder(navPath)
        }
    }

    function fileGlyph(fileName, mimeType, dir) {
        if (dir || mimeType === "application/x-nyx-directory")
            return "\uE8B7"
        const n = fileName.toLowerCase()
        if (n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".gif"))
            return "\uEB9F"
        if (n.endsWith(".mp4") || n.endsWith(".mkv") || n.endsWith(".avi"))
            return "\uE714"
        if (n.endsWith(".zip") || n.endsWith(".rar") || n.endsWith(".7z"))
            return "\uF012"
        if (mimeType.indexOf("text") >= 0)
            return "\uE8A5"
        return "\uE8A5"
    }
}
