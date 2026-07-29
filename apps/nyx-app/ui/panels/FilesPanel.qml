import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../controls"

ColumnLayout {
    id: root
    required property var theme
    required property var node

    spacing: 0

    readonly property int sectionCount: node.files.fileScopeGroupId.length > 0 ? 3 : 2
    readonly property bool narrow: width < 720 || Qt.platform.os === "android"
    property string localSearchQuery: ""
    property string remoteSearchQuery: ""
    property bool mobileFoldersOpen: false

    function matchesSearch(entry, query) {
        const q = String(query || "").trim().toLowerCase()
        if (!q.length) return true
        const name = String(entry.name || "").toLowerCase()
        const owner = String(entry.ownerLabel || "").toLowerCase()
        const path = String(entry.fullRelPath || entry.navPath || "").toLowerCase()
        return name.indexOf(q) >= 0 || owner.indexOf(q) >= 0 || path.indexOf(q) >= 0
    }

    function clampSection() {
        if (sectionTabRow.currentIndex >= sectionCount)
            sectionTabRow.currentIndex = 0
    }

    function openMobileFolders() {
        if (!narrow) return
        mobileFoldersOpen = true
        if (mobileFoldersPopup)
            mobileFoldersPopup.open()
    }

    function closeMobileFolders() {
        mobileFoldersOpen = false
        if (mobileFoldersPopup)
            mobileFoldersPopup.close()
    }

    function selectShareRoot(path) {
        node.files.setFileSelectedShareRoot(path)
        closeMobileFolders()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 56
        color: theme.bgChatHeader

        RowLayout {
            anchors.fill: parent
            anchors.margins: theme.spacing
            spacing: 10

            IconButton {
                theme: root.theme
                name: "back"
                ToolTip.text: qsTr("Назад к чату")
                onClicked: node.showChatView()
            }

            NyxIcon {
                name: "folder"
                width: 20
                height: 20
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: qsTr("Файлы")
                    color: theme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Label {
                    text: node.files.fileScopeLabel
                    color: theme.textSecondary
                    font.pixelSize: 11
                }
            }

            NyxComboBox {
                id: scopeBox
                Layout.preferredWidth: root.narrow ? Math.min(140, root.width * 0.38) : 200
                Layout.maximumWidth: root.narrow ? 160 : 280
                theme: root.theme
                model: scopeModel
                textRole: "label"
                onActivated: node.files.fileScopeGroupId = scopeModel.get(currentIndex).groupId
                Component.onCompleted: syncScopeIndex()
                function syncScopeIndex() {
                    const gid = node.files.fileScopeGroupId
                    for (let i = 0; i < scopeModel.count; ++i) {
                        if (scopeModel.get(i).groupId === gid) {
                            currentIndex = i
                            return
                        }
                    }
                    currentIndex = 0
                }
            }

            IconButton {
                theme: root.theme
                name: "refresh"
                enabled: node.files.fileExchangeReady
                ToolTip.text: qsTr("Обновить ресурсы")
                onClicked: node.files.refreshRemoteFileList()
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.border
        }
    }

    ListModel {
        id: scopeModel
        Component.onCompleted: rebuild()
        function rebuild() {
            scopeModel.clear()
            append({ label: qsTr("Личные файлы"), groupId: "" })
            for (let i = 0; i < node.groupList.length; ++i) {
                const g = node.groupList[i]
                append({ label: qsTr("Поле: %1").arg(g.name), groupId: g.groupId })
            }
            scopeBox.syncScopeIndex()
        }
    }

    Connections {
        target: node
        function onGroupListChanged() { scopeModel.rebuild() }
        function onFilesChanged() {
            scopeBox.syncScopeIndex()
            root.clampSection()
        }
    }


    Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: theme.spacing
        Layout.rightMargin: theme.spacing
        Layout.topMargin: theme.spacing
        implicitHeight: 40
        radius: theme.radiusBtn
        color: theme.inputBg
        border.color: theme.border

        Row {
            id: sectionTabRow
            anchors.fill: parent
            anchors.margins: 4
            spacing: 4
            property int currentIndex: node ? node.files.filesSection : 0

            Repeater {
                model: root.sectionCount
                delegate: Rectangle {
                    required property int index
                    width: (sectionTabRow.width - sectionTabRow.spacing * (root.sectionCount - 1))
                           / root.sectionCount
                    height: sectionTabRow.height
                    radius: theme.radiusBtn - 2
                    color: sectionTabRow.currentIndex === index ? theme.accent
                         : tabMouse.containsMouse ? theme.btnSecondaryHover : "transparent"

                    Label {
                        anchors.centerIn: parent
                        text: index === 0 ? qsTr("Обзор")
                             : index === 1 ? qsTr("Ресурсы")
                             : qsTr("Доступ")
                        color: sectionTabRow.currentIndex === index
                               ? theme.textPrimary : theme.textSecondary
                        font.pixelSize: 12
                        font.weight: sectionTabRow.currentIndex === index ? Font.DemiBold : Font.Normal
                    }

                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            node.files.setFilesSection(index)
                            if (index !== 0)
                                root.closeMobileFolders()
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: theme.spacing
        Layout.rightMargin: theme.spacing
        Layout.topMargin: 8
        spacing: 4
        visible: node.files.fileIndexProgressVisible

        Label {
            Layout.fillWidth: true
            text: node.files.fileIndexProgressLabel
            color: theme.textMuted
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }
        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 100
            value: node.files.fileIndexProgressPercent
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: theme.spacing
        Layout.rightMargin: theme.spacing
        Layout.preferredHeight: visible
                                ? Math.min(156, 28 + node.files.transferQueue.length * 48)
                                : 0
        visible: node.files.transferQueue.length > 0
        radius: theme.radiusBtn
        color: theme.inputBg
        border.color: theme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 6
            spacing: 4
            Label {
                text: qsTr("Очередь передач")
                color: theme.textSecondary
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3
                model: node.files.transferQueue
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width
                    height: 42
                    Label {
                        Layout.fillWidth: true
                        text: {
                            const dir = modelData.direction === "upload"
                                        ? qsTr("↑") : qsTr("↓")
                            let status = qsTr("ожидание")
                            if (modelData.state === "active")
                                status = modelData.progress + "%"
                            else if (modelData.state === "paused")
                                status = qsTr("пауза")
                            else if (modelData.state === "failed")
                                status = (modelData.error &&
                                          modelData.error.indexOf("источник") >= 0)
                                         ? qsTr("нет источников")
                                         : qsTr("ошибка")
                            return dir + " " + modelData.name + " · " + status
                        }
                        color: modelData.state === "failed"
                               ? "#ef5350" : theme.textPrimary
                        elide: Text.ElideMiddle
                        font.pixelSize: 11
                    }
                    ToolButton {
                        visible: modelData.direction !== "upload"
                        text: modelData.paused ? "▶" : "Ⅱ"
                        onClicked: node.files.pauseFileTransfer(modelData.hash,
                                                         !modelData.paused)
                    }
                    ToolButton {
                        visible: modelData.state === "failed"
                                 && modelData.direction !== "upload"
                        text: "↻"
                        onClicked: node.files.retryFileTransfer(modelData.hash)
                    }
                    ToolButton {
                        visible: modelData.direction !== "upload"
                        text: "↑"
                        onClicked: node.files.moveFileTransfer(modelData.hash, -1)
                    }
                    ToolButton {
                        visible: modelData.direction !== "upload"
                        text: "×"
                        onClicked: node.files.cancelFileTransfer(modelData.hash)
                    }
                }
            }
        }
    }


    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: theme.spacing

        Loader {
            id: sectionLoader
            anchors.fill: parent
            sourceComponent: sectionTabRow.currentIndex === 0 ? overviewPage
                           : sectionTabRow.currentIndex === 1 ? remotePage
                           : accessPage
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0

        ProgressBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 4
            visible: node.files.fileProgressVisible
            from: 0
            to: 100
            value: node.files.fileProgressPercent
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            visible: node.files.fileProgressVisible
            color: theme.bgInputBar
            Label {
                anchors.centerIn: parent
                text: node.files.fileProgressLabel + " · " + node.files.fileProgressPercent + "%"
                color: theme.accent
                font.pixelSize: 11
            }
        }
    }

    function openPathAccess(rootPath, relativePath, name) {
        pathAccessPopup.openForPath(rootPath, relativePath, name)
    }

    NyxMenu {
        id: rootPathMenu
        theme: root.theme
        property string path: ""
        property string displayName: ""
        property bool canRemove: true
        NyxMenuItem {
            theme: root.theme
            visible: node.files.fileScopeGroupId.length > 0 && node.files.canManageFileRoles
            text: qsTr("Права на папку…")
            onTriggered: pathAccessPopup.openForShareRoot(rootPathMenu.path, rootPathMenu.displayName)
        }
        NyxMenuItem {
            theme: root.theme
            visible: node.files.fileScopeGroupId.length > 0 && node.files.canManageFileRoles
            text: qsTr("Роли участников поля…")
            onTriggered: pathAccessPopup.openForField()
        }
        MenuSeparator {
            visible: node.files.fileScopeGroupId.length > 0 && node.files.canManageFileRoles
        }
        NyxMenuItem {
            theme: root.theme
            text: qsTr("Переиндексировать")
            onTriggered: node.files.rescanIndexedFolder(rootPathMenu.path)
        }
        NyxMenuItem {
            theme: root.theme
            text: qsTr("Убрать из индекса")
            enabled: rootPathMenu.canRemove
            onTriggered: node.files.removeIndexedFolder(rootPathMenu.path)
        }
    }

    FileAccessAssignPopup {
        id: pathAccessPopup
        parent: Overlay.overlay
        theme: root.theme
        node: root.node
    }

    Component {
        id: overviewPage

        Item {
            DropArea {
                id: dropArea
                anchors.fill: parent
                keys: ["text/uri-list"]
                onEntered: dropActive = true
                onExited: dropActive = false
                onDropped: function(drop) {
                    dropActive = false
                    drop.acceptProposedAction()
                    const urls = []
                    for (let i = 0; i < drop.urls.length; ++i)
                        urls.push(drop.urls[i])
                    node.files.addDroppedUrls(urls)
                }
            }

            property bool dropActive: false

            Rectangle {
                anchors.fill: parent
                radius: theme.radiusBtn
                color: dropArea.dropActive ? theme.accentPress : "transparent"
                border.color: dropArea.dropActive ? theme.accent : "transparent"
                border.width: 2
                opacity: dropArea.dropActive ? 0.25 : 0
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }

            RowLayout {
                anchors.fill: parent
                spacing: theme.spacing

                Rectangle {
                    id: shareRootsSidebar
                    visible: !root.narrow
                    Layout.preferredWidth: 220
                    Layout.fillHeight: true
                    radius: theme.radiusBtn
                    color: theme.bgSidebar
                    border.color: theme.border

                    Loader {
                        anchors.fill: parent
                        anchors.margins: theme.spacing
                        sourceComponent: shareRootsPane
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        visible: node.files.fileSelectedShareRoot.length === 0
                                 && node.files.localFileList.length === 0
                        wrapMode: Text.WordWrap
                        text: root.narrow
                              ? qsTr("Откройте «Мои папки» или импортируйте файлы — они появятся здесь.")
                              : (Qt.platform.os === "android"
                                 ? qsTr("Импортируйте файлы или соберите папку для обмена — они появятся здесь и в ресурсах поля.")
                                 : qsTr("Выберите папку слева или добавьте новую."))
                        color: theme.textMuted
                        font.pixelSize: 11
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: node.files.fileSelectedShareRoot.length === 0
                                 && node.files.localFileList.length > 0
                        wrapMode: Text.WordWrap
                        text: qsTr("Локальные объекты Nyx (импорт и скачанные файлы)")
                        color: theme.textSecondary
                        font.pixelSize: 11
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        IconButton {
                            visible: root.narrow
                            theme: root.theme
                            name: "folder"
                            accent: root.mobileFoldersOpen
                                    || node.files.fileSelectedShareRoot.length === 0
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Мои папки")
                            onClicked: root.openMobileFolders()
                        }

                        IconButton {
                            theme: root.theme
                            name: "back"
                            visible: node.files.fileSelectedShareRoot.length > 0
                                     || node.files.fileBrowsePath.length > 0
                            enabled: node.files.fileBrowsePath.length > 0
                                     || node.files.fileSelectedShareRoot.length > 0
                            ToolTip.text: node.files.fileBrowsePath.length > 0
                                          ? qsTr("На уровень выше")
                                          : qsTr("К списку папок")
                            onClicked: {
                                const leaveShare = node.files.fileBrowsePath.length === 0
                                        && node.files.fileSelectedShareRoot.length > 0
                                node.files.browseUp()
                                if (root.narrow && leaveShare)
                                    root.openMobileFolders()
                            }
                        }

                        Flow {
                            Layout.fillWidth: true
                            visible: node.files.fileSelectedShareRoot.length > 0
                            spacing: 4
                            Repeater {
                                model: node.files.fileBrowseCrumbs
                                delegate: Label {
                                    required property var modelData
                                    required property int index
                                    text: index > 0 ? " › " + modelData.label : modelData.label
                                    color: index === node.files.fileBrowseCrumbs.length - 1
                                           ? theme.textPrimary : theme.accent
                                    font.pixelSize: 11
                                    font.weight: index === node.files.fileBrowseCrumbs.length - 1
                                                 ? Font.DemiBold : Font.Normal
                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: function(mouse) {
                                            if (mouse.button === Qt.RightButton
                                                && index === node.files.fileBrowseCrumbs.length - 1
                                                && node.files.fileScopeGroupId.length > 0
                                                && node.files.canManageFileRoles) {
                                                root.openPathAccess(
                                                    node.files.fileSelectedShareRoot,
                                                    node.files.fileBrowsePath, modelData.label)
                                                return
                                            }
                                            if (mouse.button === Qt.LeftButton)
                                                node.files.browseToCrumb(index)
                                        }
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            visible: node.files.fileSelectedShareRoot.length === 0
                        }

                        NyxButtonSecondary {
                            visible: node.files.fileScopeGroupId.length > 0 && node.files.canManageFileRoles
                            theme: root.theme
                            text: qsTr("Роли поля")
                            ToolTip.text: qsTr("Роли участников по умолчанию")
                            onClicked: pathAccessPopup.openForField()
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        visible: node.files.fileScopeGroupId.length > 0
                        spacing: 6
                        Repeater {
                            model: node.files.fileMemberAccess
                            delegate: Rectangle {
                                required property string nickname
                                required property string roleName
                                required property bool isOwner
                                radius: 10
                                height: 24
                                width: chipLbl.implicitWidth + 16
                                color: theme.inputBg
                                border.color: theme.border
                                Label {
                                    id: chipLbl
                                    anchors.centerIn: parent
                                    text: nickname + (isOwner ? qsTr(" · влад.") : (" · " + roleName))
                                    color: theme.textSecondary
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: node.files.fileSelectedShareRoot.length > 0
                        wrapMode: Text.WordWrap
                        text: node.files.fileScopeGroupId.length === 0
                              ? qsTr("Щелчок по папке — войти внутрь. Перетащите папку или файл.")
                              : qsTr("ПКМ по папке или файлу — назначить роль. Замок на строке — то же. «Роли поля» — роли участников по умолчанию.")
                        color: theme.textMuted
                        font.pixelSize: 11
                    }

                    NyxTextField {
                        Layout.fillWidth: true
                        theme: root.theme
                        placeholderText: qsTr("Поиск файлов и папок…")
                        text: root.localSearchQuery
                        onTextChanged: root.localSearchQuery = text
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ListView {
                            id: localList
                            anchors.fill: parent
                            clip: true
                            spacing: 4
                            model: node.files.localFileList
                            delegate: Item {
                                required property var modelData
                                width: ListView.view ? ListView.view.width : parent.width
                                height: visible ? fileRow.height : 0
                                visible: root.matchesSearch(modelData, root.localSearchQuery)

                                FileListRow {
                                    id: fileRow
                                    width: parent.width
                                    theme: root.theme
                                    node: root.node
                                    fileName: modelData.name || ""
                                    fileHash: modelData.hash || ""
                                    fileSizeLabel: modelData.sizeLabel || ""
                                    fileSize: modelData.size || 0
                                    fileMime: modelData.mime || ""
                                    fileIsRemote: modelData.isRemote === true
                                    fileIsDirectory: modelData.isDirectory === true
                                                     || modelData.mime === "application/x-nyx-directory"
                                    fileNavPath: modelData.navPath || ""
                                    fileRootPath: modelData.rootPath || ""
                                    fileFullRelPath: modelData.fullRelPath || modelData.navPath || ""
                                    fileOwnerLabel: modelData.ownerLabel || ""
                                    onAccessContextMenuRequested: root.openPathAccess(
                                        fileRow.fileRootPath, fileRow.fileFullRelPath, fileRow.fileName)
                                    NyxButtonSecondary {
                                    visible: true
                                    theme: root.theme
                                    text: qsTr("В чат")
                                    onClicked: {
                                        if (fileRow.fileIsDirectory) {
                                            node.files.linkFolderToChat(
                                                fileRow.fileHash, fileRow.fileName,
                                                fileRow.fileRootPath, fileRow.fileFullRelPath,
                                                fileRow.fileSize)
                                        } else {
                                            node.files.linkFileToChat(
                                                fileRow.fileHash, fileRow.fileName,
                                                fileRow.fileMime, fileRow.fileSize)
                                        }
                                    }
                                }
                                NyxButtonSecondary {
                                    visible: !fileRow.fileIsDirectory
                                    theme: root.theme
                                    text: qsTr("Открыть")
                                    onClicked: node.files.openFileByHash(
                                        fileRow.fileHash, fileRow.fileName, fileRow.fileMime,
                                        fileRow.fileRootPath, fileRow.fileFullRelPath)
                                }
                                NyxButtonSecondary {


                                    visible: node.files.fileScopeGroupId.length === 0
                                             && node.files.fileExchangeReady
                                             && node.files.canFileUpload
                                             && !fileRow.fileIsDirectory
                                    theme: root.theme
                                    text: qsTr("Передать")
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Отправить этот файл собеседнику в текущем чате (не в поле)")
                                    onClicked: node.files.sendFileByHash(fileRow.fileHash)
                                }
                                NyxButtonSecondary {
                                    visible: Qt.platform.os === "android"
                                             && !fileRow.fileIsDirectory
                                    theme: root.theme
                                    text: qsTr("Экспорт")
                                    onClicked: node.files.exportFile(
                                        fileRow.fileHash, fileRow.fileName,
                                        fileRow.fileMime)
                                }
                                IconButton {
                                    visible: node.files.fileScopeGroupId.length > 0 && node.files.canManageFileRoles
                                    theme: root.theme
                                    name: "lock"
                                    ToolTip.text: qsTr("Права доступа")
                                    onClicked: pathAccessPopup.openForPath(
                                        fileRow.fileRootPath, fileRow.fileFullRelPath, fileRow.fileName)
                                }
                                }
                            }
                        }

                        EmptyState {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: localList.count === 0 && node.files.fileShareRoots.length === 0
                            theme: root.theme
                            emoji: "📁"
                            title: qsTr("Нет папок")
                            hint: Qt.platform.os === "android"
                                  ? qsTr("Импортируйте файлы из Documents, Downloads или облачного хранилища")
                                  : qsTr("Добавьте или перетащите папку")
                        }

                        EmptyState {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: localList.count === 0 && node.files.fileShareRoots.length > 0
                                     && node.files.fileSelectedShareRoot.length > 0
                            theme: root.theme
                            emoji: "📂"
                            title: qsTr("Папка пуста")
                            hint: qsTr("Положите файлы и нажмите «Переиндексировать» (ПКМ по корню)")
                        }

                        EmptyState {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: root.narrow
                                     && localList.count === 0
                                     && node.files.fileShareRoots.length > 0
                                     && node.files.fileSelectedShareRoot.length === 0
                            theme: root.theme
                            emoji: "📁"
                            title: qsTr("Выберите папку")
                            hint: qsTr("Нажмите значок «Мои папки» сверху")
                        }
                    }
                }
            }
        }
    }

    Component {
        id: shareRootsPane

        ColumnLayout {
            spacing: 8

            Label {
                text: qsTr("Мои папки")
                visible: !root.narrow
                color: theme.textSecondary
                font.pixelSize: 11
                font.capitalization: Font.AllUppercase
            }

            Label {
                Layout.fillWidth: true
                visible: node.files.fileScopeGroupId.length > 0
                wrapMode: Text.WordWrap
                text: qsTr("Только ваши share-папки в этом поле. Каталог других участников — вкладка «Ресурсы».")
                color: theme.textMuted
                font.pixelSize: 10
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: node.files.fileShareRoots
                delegate: ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: rowLayout.implicitHeight + 16
                    padding: 8
                    highlighted: node.files.fileSelectedShareRoot === modelData.path
                    background: Rectangle {
                        radius: theme.radiusBtn - 2
                        color: parent.highlighted ? theme.accentPress
                             : parent.hovered ? theme.btnSecondaryHover : theme.btnSecondary
                        border.color: parent.highlighted ? theme.accent : theme.border
                        border.width: parent.highlighted ? 1 : 0
                    }
                    contentItem: RowLayout {
                        id: rowLayout
                        spacing: 8
                        NyxIcon {
                            Layout.alignment: Qt.AlignVCenter
                            name: "folder"
                            width: 18
                            height: 18
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 1
                            Label {
                                Layout.fillWidth: true
                                text: modelData.displayName || modelData.path
                                color: theme.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                ToolTip.text: modelData.path
                                ToolTip.visible: hovered
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: (modelData.scopeLabel || "").length > 0
                                text: modelData.scopeLabel
                                color: theme.textMuted
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }
                        Label {
                            Layout.alignment: Qt.AlignVCenter
                            text: modelData.fileCount === 0 ? qsTr("пусто")
                                  : qsTr("%1 ф.").arg(modelData.fileCount)
                            color: theme.textMuted
                            font.pixelSize: 10
                        }
                        IconButton {
                            Layout.alignment: Qt.AlignVCenter
                            theme: root.theme
                            name: "delete"
                            visible: modelData.canRemove === true
                            ToolTip.text: qsTr("Убрать из индекса")
                            onClicked: node.files.removeIndexedFolder(modelData.path)
                        }
                    }
                    onClicked: root.selectShareRoot(modelData.path)
                    onPressAndHold: {
                        rootPathMenu.path = modelData.path
                        rootPathMenu.displayName = modelData.displayName || modelData.path
                        rootPathMenu.canRemove = modelData.canRemove === true
                        rootPathMenu.popup()
                    }
                }
            }

            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                enabled: node.files.canAddShareFolder
                text: Qt.platform.os === "android"
                      ? qsTr("Папка для обмена…")
                      : qsTr("Добавить папку…")
                onClicked: node.files.addIndexedFolder("")
            }
            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                enabled: node.files.canAddShareFolder
                text: qsTr("Импортировать файлы…")
                onClicked: node.files.importFiles()
            }
        }
    }

    Item {
        Layout.preferredWidth: 0
        Layout.preferredHeight: 0
        Layout.maximumHeight: 0
        width: 0
        height: 0

        Popup {
            id: mobileFoldersPopup
            parent: Overlay.overlay
            modal: true
            focus: true
            anchors.centerIn: Overlay.overlay
            width: Overlay.overlay ? Overlay.overlay.width : root.width
            height: Overlay.overlay ? Overlay.overlay.height : root.height
            padding: 0
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            onClosed: root.mobileFoldersOpen = false

            background: Rectangle { color: theme.bgApp }

            contentItem: ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    color: theme.bgChatHeader
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: theme.spacing
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Мои папки")
                            color: theme.textPrimary
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        IconButton {
                            theme: root.theme
                            name: "close"
                            onClicked: root.closeMobileFolders()
                        }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: theme.border
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: theme.spacing
                    active: mobileFoldersPopup.opened
                    sourceComponent: shareRootsPane
                }
            }
        }
    }

    Component {
        id: remotePage

        ColumnLayout {
            anchors.fill: parent
            spacing: theme.spacing

            Label {
                Layout.fillWidth: true
                visible: !node.files.fileExchangeReady
                wrapMode: Text.WordWrap
                text: node.files.fileExchangeHint.length
                      ? node.files.fileExchangeHint
                      : qsTr("Подключитесь к чату или войдите в поле, затем нажмите «Обновить» в шапке.")
                color: theme.textMuted
                font.pixelSize: 11
            }

            Label {
                Layout.fillWidth: true
                visible: node.files.fileExchangeReady
                wrapMode: Text.WordWrap
                text: qsTr("Общие папки участников поля. Нажмите «Обновить» в шапке, если список пуст.")
                color: theme.textMuted
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: node.files.fileExchangeReady
                spacing: 6
                IconButton {
                    theme: root.theme
                    name: "folder"
                    enabled: node.files.fileResourcesRoot.length > 0
                             || node.files.fileRemoteBrowsePath.length > 0
                    ToolTip.text: node.files.fileRemoteBrowsePath.length > 0
                                  ? qsTr("На уровень выше")
                                  : qsTr("К списку папок поля")
                    onClicked: node.files.browseUp()
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: node.files.fileRemoteBrowseCrumbs
                        delegate: Label {
                            required property var modelData
                            required property int index
                            text: index > 0 ? " › " + modelData.label : modelData.label
                            color: index === node.files.fileRemoteBrowseCrumbs.length - 1
                                   ? theme.textPrimary : theme.accent
                            font.pixelSize: 11
                            font.weight: index === node.files.fileRemoteBrowseCrumbs.length - 1
                                         ? Font.DemiBold : Font.Normal
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: node.files.browseToCrumb(index)
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: node.files.fileExchangeReady
                wrapMode: Text.WordWrap
                text: node.files.fileResourcesRoot.length === 0
                      ? qsTr("Щелчок по папке — открыть share-папку участника.")
                      : qsTr("Щелчок по папке — войти внутрь. Скачайте файл кнопкой справа.")
                color: theme.textMuted
                font.pixelSize: 11
            }

            NyxTextField {
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Поиск по имени или владельцу…")
                text: root.remoteSearchQuery
                onTextChanged: root.remoteSearchQuery = text
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: remoteList
                    anchors.fill: parent
                    clip: true
                    spacing: 4
                    visible: node.files.canFileList
                    model: node.files.remoteFileList
                    delegate: Item {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : parent.width
                        height: visible ? fileRow.height : 0
                        visible: root.matchesSearch(modelData, root.remoteSearchQuery)

                        FileListRow {
                            id: fileRow
                            width: parent.width
                            theme: root.theme
                            node: root.node
                            fileName: modelData.name || ""
                            fileHash: modelData.hash || ""
                            fileSizeLabel: modelData.sizeLabel || ""
                            fileSize: modelData.size || 0
                            fileMime: modelData.mime || ""
                            fileIsRemote: modelData.isRemote === true
                            fileIsDirectory: modelData.isDirectory === true
                                             || modelData.mime === "application/x-nyx-directory"
                            fileNavPath: modelData.navPath || ""
                            fileRootPath: modelData.rootPath || ""
                            fileFullRelPath: modelData.fullRelPath || modelData.navPath || ""
                            fileOwnerLabel: modelData.ownerLabel || ""
                            NyxButtonSecondary {
                                visible: true
                                theme: root.theme
                                text: qsTr("В чат")
                                onClicked: {
                                    if (fileRow.fileIsDirectory) {
                                        node.files.linkFolderToChat(
                                            fileRow.fileHash, fileRow.fileName,
                                            fileRow.fileRootPath, fileRow.fileFullRelPath,
                                            fileRow.fileSize)
                                    } else {
                                        node.files.linkFileToChat(
                                            fileRow.fileHash, fileRow.fileName,
                                            fileRow.fileMime, fileRow.fileSize)
                                    }
                                }
                            }
                            NyxButtonSecondary {
                                visible: fileRow.fileIsDirectory && modelData.canDownload === true
                                theme: root.theme
                                text: qsTr("Скачать папку")
                                onClicked: node.files.downloadRemoteFolder(fileRow.fileRootPath, fileRow.fileFullRelPath)
                            }
                            NyxButtonSecondary {
                                visible: !fileRow.fileIsDirectory && modelData.canDownload === true
                                theme: root.theme
                                text: qsTr("Скачать")
                                onClicked: node.files.downloadFile(fileRow.fileHash, fileRow.fileName, fileRow.fileRootPath, fileRow.fileFullRelPath)
                            }
                            NyxButtonSecondary {
                                visible: !fileRow.fileIsDirectory && modelData.canOpenRemote === true
                                theme: root.theme
                                text: qsTr("Открыть")
                                onClicked: node.files.openRemoteFile(fileRow.fileHash, fileRow.fileName, fileRow.fileRootPath, fileRow.fileFullRelPath)
                            }
                        }
                    }
                }

                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: !node.files.canFileList && node.files.fileExchangeReady
                    theme: root.theme
                    emoji: "🔒"
                    title: qsTr("Нет доступа к списку")
                    hint: qsTr("Обратитесь к владельцу поля")
                }

                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: remoteList.count === 0 && node.files.fileExchangeReady && node.files.canFileList
                    theme: root.theme
                    emoji: "🌐"
                    title: qsTr("Список пуст")
                    hint: qsTr("Обновите список в шапке")
                }
            }
        }
    }

    Component {
        id: accessPage

        ScrollView {
            id: accessScroll
            anchors.fill: parent
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                id: accessCol
                width: accessScroll.availableWidth
                spacing: theme.spacing

                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: node.files.canManageFileRoles
                          ? qsTr("Пресеты прав — конструктор масок. Роли собираются из прав. Назначение ролей — на «Обзор».")
                          : qsTr("Права на файлы настраивает владелец поля.")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                Label {
                    text: qsTr("Пресеты прав")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: node.files.filePermissionPresetList
                    delegate: Rectangle {
                        required property string presetId
                        required property string name
                        required property int permissions
                        width: parent.width
                        implicitHeight: presetCol.implicitHeight + 16
                        radius: theme.radiusBtn
                        color: theme.btnSecondary
                        border.color: theme.border
                        Column {
                            id: presetCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            spacing: 6
                            width: parent.width
                            Label {
                                width: parent.width
                                text: name
                                color: theme.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            Flow {
                                width: parent.width
                                spacing: 6
                                Repeater {
                                    model: [
                                        { bit: node.files.permFileList, label: qsTr("Список") },
                                        { bit: node.files.permFileDownload, label: qsTr("Скачать") },
                                        { bit: node.files.permFileUpload, label: qsTr("Отправить") },
                                        { bit: node.files.permFileDelete, label: qsTr("Удалить") },
                                        { bit: node.files.permFileOpenRemote, label: qsTr("По сети") },
                                        { bit: node.files.permFileManageShares, label: qsTr("Папки") },
                                        { bit: node.files.permFileManageRoles, label: qsTr("Роли") }
                                    ]
                                    delegate: Rectangle {
                                        required property int bit
                                        required property string label
                                        radius: 10
                                        height: 22
                                        width: ptChip.implicitWidth + 16
                                        color: (permissions & bit) ? theme.accent : theme.inputBg
                                        border.color: theme.border
                                        Label {
                                            id: ptChip
                                            anchors.centerIn: parent
                                            text: label
                                            font.pixelSize: 10
                                            color: (permissions & bit) ? theme.textPrimary : theme.textMuted
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: node.files.canManageFileRoles
                                            onClicked: node.files.togglePermissionPresetBit(presetId, bit)
                                        }
                                    }
                                }
                            }
                            Row {
                                width: parent.width
                                spacing: 8
                                Item { width: parent.width - delBtn.width - 8; height: 1 }
                                NyxButtonSecondary {
                                    id: delBtn
                                    theme: root.theme
                                    text: qsTr("Удалить")
                                    onClicked: node.files.deletePermissionPreset(presetId)
                                }
                            }
                        }
                    }
                }

                Row {
                    width: parent.width
                    spacing: 8
                    visible: node.files.canManageFileRoles
                    NyxTextField {
                        id: newPresetName
                        width: parent.width - createPresetBtn.width - 8
                        theme: root.theme
                        placeholderText: qsTr("Имя пресета прав")
                    }
                    NyxButtonSecondary {
                        id: createPresetBtn
                        theme: root.theme
                        text: qsTr("Создать пресет")
                        enabled: newPresetName.text.trim().length > 0
                        onClicked: {
                            node.files.createPermissionPreset(newPresetName.text,
                                node.files.permFileList | node.files.permFileDownload)
                            newPresetName.clear()
                        }
                    }
                }

                Label {
                    text: qsTr("Роли")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: node.files.fileRoleList
                    delegate: Rectangle {
                        required property string roleId
                        required property string name
                        required property int permissions
                        required property bool builtin
                        width: parent.width
                        implicitHeight: roleCol.implicitHeight + 16
                        radius: theme.radiusBtn
                        color: theme.btnSecondary
                        border.color: theme.border
                        Column {
                            id: roleCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            spacing: 6
                            width: parent.width
                            Label {
                                width: parent.width
                                text: name + (builtin ? qsTr(" · встроенная") : "")
                                color: theme.textPrimary
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Flow {
                                width: parent.width
                                spacing: 6
                                Repeater {
                                    model: [
                                        { bit: node.files.permFileList, label: qsTr("Список") },
                                        { bit: node.files.permFileDownload, label: qsTr("Скачать") },
                                        { bit: node.files.permFileUpload, label: qsTr("Отправить") },
                                        { bit: node.files.permFileDelete, label: qsTr("Удалить") },
                                        { bit: node.files.permFileOpenRemote, label: qsTr("По сети") },
                                        { bit: node.files.permFileManageShares, label: qsTr("Папки") },
                                        { bit: node.files.permFileManageRoles, label: qsTr("Роли") }
                                    ]
                                    delegate: Rectangle {
                                        required property int bit
                                        required property string label
                                        radius: 10
                                        height: 22
                                        width: chipText.implicitWidth + 16
                                        color: (permissions & bit) ? theme.accent : theme.inputBg
                                        border.color: theme.border
                                        Label {
                                            id: chipText
                                            anchors.centerIn: parent
                                            text: label
                                            color: (permissions & bit) ? theme.textPrimary : theme.textMuted
                                            font.pixelSize: 10
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: node.files.canManageFileRoles
                                                     && node.files.canEditFileRolePermissions(roleId)
                                            onClicked: node.files.toggleFileRolePermission(roleId, bit)
                                        }
                                    }
                                }
                            }
                            Row {
                                width: parent.width
                                spacing: 8
                                visible: node.files.canManageFileRoles && node.files.filePermissionPresetList.length > 0
                                         && node.files.canEditFileRolePermissions(roleId)
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Из пресета:")
                                    color: theme.textMuted
                                    font.pixelSize: 10
                                }
                                Repeater {
                                    model: node.files.filePermissionPresetList
                                    delegate: NyxButtonSecondary {
                                        required property string presetId
                                        required property string name
                                        theme: root.theme
                                        text: name
                                        onClicked: node.files.applyPresetToRole(presetId, roleId)
                                    }
                                }
                            }
                            Row {
                                width: parent.width
                                visible: node.files.canManageFileRoles && !builtin
                                spacing: 8
                                Item { width: parent.width - delRoleBtn.width; height: 1 }
                                NyxButtonSecondary {
                                    id: delRoleBtn
                                    theme: root.theme
                                    text: qsTr("Удалить роль")
                                    onClicked: node.files.deleteFileRole(roleId)
                                }
                            }
                        }
                    }
                }

                Row {
                    width: parent.width
                    spacing: 8
                    visible: node.files.canManageFileRoles
                    NyxTextField {
                        id: newRoleName
                        width: parent.width - createRoleBtn.width - 8
                        theme: root.theme
                        placeholderText: qsTr("Имя новой роли")
                    }
                    NyxButtonSecondary {
                        id: createRoleBtn
                        theme: root.theme
                        text: qsTr("Создать роль")
                        enabled: newRoleName.text.trim().length > 0
                        onClicked: {
                            node.files.createFileRole(newRoleName.text,
                                                node.files.permFileList | node.files.permFileDownload)
                            newRoleName.clear()
                        }
                    }
                }

                Label {
                    text: qsTr("Участники поля")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: node.files.fileMemberAccess
                    delegate: Rectangle {
                        required property string userId
                        required property string nickname
                        required property string idShort
                        required property string roleId
                        required property bool isOwner
                        width: parent.width
                        implicitHeight: 44
                        radius: theme.radiusBtn
                        color: theme.btnSecondary
                        border.color: theme.border
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: nickname + " · " + idShort + (isOwner ? qsTr(" · влад.") : "")
                                color: theme.textPrimary
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            NyxComboBox {
                                id: memberRoleCombo
                                Layout.preferredWidth: 170
                                visible: !isOwner && node.files.canManageFileRoles
                                theme: root.theme
                                model: node.files.fileRoleList
                                textRole: "name"
                                Component.onCompleted: syncRole()
                                function syncRole() {
                                    const roles = node.files.fileRoleList
                                    for (let i = 0; i < roles.length; ++i) {
                                        if (roles[i].roleId === roleId) {
                                            currentIndex = i
                                            return
                                        }
                                    }
                                    currentIndex = roles.length > 0 ? 0 : -1
                                }
                                onActivated: {
                                    const roles = node.files.fileRoleList
                                    if (currentIndex >= 0 && currentIndex < roles.length)
                                        node.files.setMemberFileRole(userId, roles[currentIndex].roleId)
                                }
                                Connections {
                                    target: node
                                    function onFileAccessChanged() { memberRoleCombo.syncRole() }
                                }
                            }
                        }
                    }
                }

                Item { height: theme.spacing }
            }
        }
    }
}
