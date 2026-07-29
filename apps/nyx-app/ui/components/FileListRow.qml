import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "."

/** File or folder row in the browser. */
Rectangle {
    id: root
    required property var theme
    property string fileName: ""
    property string fileHash: ""
    property string fileSizeLabel: ""
    property var fileSize: 0
    property string fileMime: ""
    property bool fileIsRemote: false
    property bool fileIsDirectory: false
    property string fileNavPath: ""
    property string fileRootPath: ""
    property string fileFullRelPath: ""
    property string fileOwnerLabel: ""
    property var node
    /** Right click on the row assigns permissions (fields only). */
    signal accessContextMenuRequested()

    readonly property bool compact: width < 480 || Qt.platform.os === "android"

    default property alias actions: actionsLayout.data

    width: ListView.view ? ListView.view.width : parent.width
    height: contentGrid.implicitHeight + 16
    radius: theme.radiusBtn
    color: mouseArea.containsMouse ? theme.btnSecondaryHover : theme.btnSecondary
    border.color: fileIsDirectory ? theme.accent : theme.border
    border.width: fileIsDirectory ? 1 : 1

    GridLayout {
        id: contentGrid
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 10
        columns: root.compact ? 1 : 2
        columnSpacing: 10
        rowSpacing: 8

        RowLayout {
            id: nameRow
            Layout.fillWidth: true
            Layout.column: 0
            Layout.row: 0
            Layout.minimumWidth: 40
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
                    text: {
                        let bits = []
                        if (fileOwnerLabel.length)
                            bits.push(fileOwnerLabel)
                        if (fileIsDirectory)
                            bits.push(fileSizeLabel)
                        else
                            bits.push(fileSizeLabel + " · " + fileMime)
                        return bits.filter(function(s) { return s && s.length }).join(" · ")
                    }
                    color: theme.textMuted
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    visible: fileSizeLabel.length > 0 || fileOwnerLabel.length > 0
                }
            }
        }

        Flow {
            id: actionsLayout
            Layout.column: root.compact ? 0 : 1
            Layout.row: root.compact ? 1 : 0
            Layout.fillWidth: root.compact
            Layout.alignment: root.compact
                             ? (Qt.AlignLeft | Qt.AlignVCenter)
                             : (Qt.AlignRight | Qt.AlignVCenter)
            spacing: 6
            z: 1
        }
    }

    MouseArea {
        id: mouseArea
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.compact
               ? parent.width
               : Math.max(0, parent.width - actionsLayout.width - 16)
        height: root.compact
                ? Math.min(parent.height, nameRow.height + 20)
                : parent.height
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                if (node && node.fileScopeGroupId.length > 0 && node.canManageFileRoles)
                    root.accessContextMenuRequested()
                return
            }
            if (fileIsDirectory && node) {
                node.browseIntoFolder(fileNavPath, fileRootPath)
                return
            }
            if (node && fileHash.length)
                node.openFileByHash(fileHash, fileName, fileMime, fileRootPath, fileFullRelPath)
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
        if (n.endsWith(".mp3") || n.endsWith(".wav") || n.endsWith(".ogg") || n.endsWith(".m4a")
                || mimeType.indexOf("audio") >= 0)
            return "mic"
        return "file"
    }
}
