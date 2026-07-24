import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "."

/** Строка файла или папки в браузере. */
Rectangle {
    id: root
    required property var theme
    property string fileName: ""
    property string fileHash: ""
    property string fileSizeLabel: ""
    property string fileMime: ""
    property bool fileIsRemote: false
    property bool fileIsDirectory: false
    property string fileNavPath: ""
    property string fileRootPath: ""
    property string fileFullRelPath: ""
    property var node
    /** ПКМ по строке — назначение прав (только в поле). */
    signal accessContextMenuRequested()

    default property alias actions: actionsLayout.data

    width: ListView.view ? ListView.view.width : parent.width
    height: row.implicitHeight + 16
    radius: theme.radiusBtn
    color: mouseArea.containsMouse ? theme.btnSecondaryHover : theme.btnSecondary
    border.color: fileIsDirectory ? theme.accent : theme.border
    border.width: fileIsDirectory ? 1 : 1

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10
        z: 1

        NyxIcon {
            Layout.alignment: Qt.AlignVCenter
            name: fileIconName(fileName, fileMime, fileIsDirectory)
            width: 20
            height: 20
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            Layout.minimumWidth: 40
            spacing: 2
            Text {
                text: fileName.length > 0 ? fileName : qsTr("(без имени)")
                color: fileName.length > 0 ? theme.textPrimary : theme.textMuted
                font.pixelSize: 13
                font.weight: fileIsDirectory ? Font.DemiBold : Font.Normal
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            Text {
                text: fileIsDirectory ? fileSizeLabel : (fileSizeLabel + " · " + fileMime)
                color: theme.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
                Layout.fillWidth: true
                visible: fileSizeLabel.length > 0
            }
        }

        RowLayout {
            id: actionsLayout
            Layout.alignment: Qt.AlignVCenter
            spacing: 6
        }
    }

    MouseArea {
        id: mouseArea
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.max(0, parent.width - actionsLayout.width - 16)
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: fileIsDirectory ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                if (node && node.fileScopeGroupId.length > 0 && node.canManageFileRoles)
                    root.accessContextMenuRequested()
                return
            }
            if (fileIsDirectory && node)
                node.browseIntoFolder(fileNavPath, fileRootPath)
        }
    }

    function fileIconName(entryName, mimeType, dir) {
        if (dir || mimeType === "application/x-nyx-directory")
            return "folder"
        const n = entryName.toLowerCase()
        if (n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".gif")
                || n.endsWith(".webp") || mimeType.indexOf("image") >= 0)
            return "image"
        if (n.endsWith(".mp4") || n.endsWith(".mkv") || n.endsWith(".avi") || mimeType.indexOf("video") >= 0)
            return "video"
        return "file"
    }
}
