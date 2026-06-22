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

    readonly property int sectionCount: node.fileScopeGroupId.length > 0 ? 3 : 2

    function clampSection() {
        if (sectionTabRow.currentIndex >= sectionCount)
            sectionTabRow.currentIndex = 0
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
                glyph: "\uE72B"
                ToolTip.text: qsTr("Назад к чату")
                onClicked: node.showChatView()
            }

            Text {
                text: "\uE8B7"
                font.family: "Segoe MDL2 Assets"
                font.pixelSize: 20
                color: theme.accent
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
                    text: node.fileScopeLabel
                    color: theme.textSecondary
                    font.pixelSize: 11
                }
            }

            NyxComboBox {
                id: scopeBox
                Layout.preferredWidth: 200
                theme: root.theme
                model: scopeModel
                textRole: "label"
                onActivated: node.fileScopeGroupId = scopeModel.get(currentIndex).groupId
                Component.onCompleted: syncScopeIndex()
                function syncScopeIndex() {
                    const gid = node.fileScopeGroupId
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
                glyph: "\uE72C"
                enabled: node.fileExchangeReady
                ToolTip.text: qsTr("Обновить ресурсы")
                onClicked: node.refreshRemoteFileList()
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

    // --- вкладки: явный Row, без скрытых TabButton ---
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
            property int currentIndex: 0

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
                        onClicked: sectionTabRow.currentIndex = index
                    }
                }
            }
        }
    }

    // --- содержимое: только одна страница через Loader ---
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
            visible: node.fileProgressVisible
            from: 0
            to: 100
            value: node.fileProgressPercent
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            visible: node.fileProgressVisible
            color: theme.bgInputBar
            Label {
                anchors.centerIn: parent
                text: node.fileProgressLabel + " · " + node.fileProgressPercent + "%"
                color: theme.accent
                font.pixelSize: 11
            }
        }
    }

    Menu {
        id: rootPathMenu
        property string path: ""
        property bool canRemove: true
        MenuItem {
            text: qsTr("Переиндексировать")
            onTriggered: node.rescanIndexedFolder(rootPathMenu.path)
        }
        MenuItem {
            text: qsTr("Убрать из индекса")
            enabled: rootPathMenu.canRemove
            onTriggered: node.removeIndexedFolder(rootPathMenu.path)
        }
    }

    // ===== Страница: Обзор =====
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
                    node.addDroppedUrls(urls)
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
                    Layout.preferredWidth: 220
                    Layout.fillHeight: true
                    radius: theme.radiusBtn
                    color: theme.bgSidebar
                    border.color: theme.border

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: theme.spacing
                        spacing: 8

                        Label {
                            text: qsTr("Мои папки")
                            color: theme.textSecondary
                            font.pixelSize: 11
                            font.capitalization: Font.AllUppercase
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 4
                            model: node.fileShareRoots
                            delegate: ItemDelegate {
                                required property var modelData
                                width: ListView.view.width
                                height: 36
                                padding: 8
                                highlighted: node.fileSelectedShareRoot === modelData.path
                                background: Rectangle {
                                    radius: theme.radiusBtn - 2
                                    color: parent.highlighted ? theme.accentPress
                                         : parent.hovered ? theme.btnSecondaryHover : theme.btnSecondary
                                    border.color: parent.highlighted ? theme.accent : theme.border
                                    border.width: parent.highlighted ? 1 : 0
                                }
                                contentItem: RowLayout {
                                    spacing: 6
                                    Text {
                                        text: "\uE8B7"
                                        font.family: "Segoe MDL2 Assets"
                                        color: theme.accent
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.path
                                            color: theme.textPrimary
                                            font.pixelSize: 11
                                            elide: Text.ElideMiddle
                                        }
                                        Label {
                                            visible: modelData.scopeLabel !== undefined
                                            text: modelData.scopeLabel || ""
                                            color: theme.textMuted
                                            font.pixelSize: 9
                                        }
                                    }
                                    Label {
                                        text: modelData.fileCount === 0 ? qsTr("пусто")
                                              : qsTr("%1 ф.").arg(modelData.fileCount)
                                        color: theme.textMuted
                                        font.pixelSize: 10
                                    }
                                    IconButton {
                                        theme: root.theme
                                        glyph: "\uE74D"
                                        visible: modelData.canRemove === true
                                        ToolTip.text: qsTr("Убрать из индекса")
                                        onClicked: node.removeIndexedFolder(modelData.path)
                                    }
                                }
                                onClicked: node.setFileSelectedShareRoot(modelData.path)
                                onPressAndHold: {
                                    rootPathMenu.path = modelData.path
                                    rootPathMenu.canRemove = modelData.canRemove === true
                                    rootPathMenu.popup()
                                }
                            }
                        }

                        NyxButtonSecondary {
                            Layout.fillWidth: true
                            theme: root.theme
                            enabled: node.canAddShareFolder
                            text: qsTr("Добавить папку…")
                            onClicked: node.addIndexedFolder("")
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        visible: node.fileSelectedShareRoot.length === 0
                        wrapMode: Text.WordWrap
                        text: qsTr("Выберите папку слева или добавьте новую.")
                        color: theme.textMuted
                        font.pixelSize: 11
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: node.fileSelectedShareRoot.length > 0
                        spacing: 6

                        IconButton {
                            theme: root.theme
                            glyph: "\uE74B"
                            enabled: node.fileBrowsePath.length > 0
                            ToolTip.text: qsTr("На уровень выше")
                            onClicked: node.browseUp()
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: 4
                            Repeater {
                                model: node.fileBrowseCrumbs
                                delegate: Label {
                                    required property var modelData
                                    required property int index
                                    text: index > 0 ? " › " + modelData.label : modelData.label
                                    color: index === node.fileBrowseCrumbs.length - 1
                                           ? theme.textPrimary : theme.accent
                                    font.pixelSize: 11
                                    font.weight: index === node.fileBrowseCrumbs.length - 1
                                                 ? Font.DemiBold : Font.Normal
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: node.browseToCrumb(index)
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: node.fileSelectedShareRoot.length > 0
                        wrapMode: Text.WordWrap
                        text: node.fileScopeGroupId.length === 0
                              ? qsTr("Щелчок по папке — войти внутрь. Перетащите папку или файл.")
                              : qsTr("Щелчок — войти в папку. Участники видят файлы на «Ресурсы».")
                        color: theme.textMuted
                        font.pixelSize: 11
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        visible: node.fileIndexProgressVisible
                        from: 0
                        to: 100
                        value: node.fileIndexProgressPercent
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: node.fileIndexProgressVisible
                        text: node.fileIndexProgressLabel
                        color: theme.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ListView {
                            id: localList
                            anchors.fill: parent
                            clip: true
                            spacing: 4
                            model: node.localFiles
                            delegate: FileListRow {
                                theme: root.theme
                                node: root.node
                                name: model.name
                                hash: model.hash
                                sizeLabel: model.sizeLabel
                                mime: model.mime
                                isRemote: model.isRemote
                                isDirectory: model.isDirectory
                                navPath: model.navPath
                                rootPath: model.rootPath
                                NyxButtonSecondary {
                                    visible: node.fileExchangeReady && node.canFileUpload && !isDirectory
                                    theme: root.theme
                                    text: qsTr("Отправить")
                                    onClicked: node.sendFileByHash(hash)
                                }
                            }
                        }

                        EmptyState {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: localList.count === 0 && node.fileShareRoots.length === 0
                            theme: root.theme
                            emoji: "📁"
                            title: qsTr("Нет папок")
                            hint: qsTr("Добавьте или перетащите папку")
                        }

                        EmptyState {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: localList.count === 0 && node.fileShareRoots.length > 0
                                     && node.fileSelectedShareRoot.length > 0
                            theme: root.theme
                            emoji: "📂"
                            title: qsTr("Папка пуста")
                            hint: qsTr("Положите файлы и нажмите «Переиндексировать» (ПКМ по корню)")
                        }
                    }
                }
            }
        }
    }

    // ===== Страница: Ресурсы соседа =====
    Component {
        id: remotePage

        ColumnLayout {
            anchors.fill: parent
            spacing: theme.spacing

            Label {
                Layout.fillWidth: true
                visible: !node.fileExchangeReady
                wrapMode: Text.WordWrap
                text: node.fileExchangeHint.length
                      ? node.fileExchangeHint
                      : qsTr("Подключитесь к чату или войдите в поле, затем нажмите «Обновить» в шапке.")
                color: theme.textMuted
                font.pixelSize: 11
            }

            Label {
                Layout.fillWidth: true
                visible: node.fileExchangeReady && node.fileSelectedShareRoot.length === 0
                wrapMode: Text.WordWrap
                text: qsTr("Выберите свою папку слева на «Обзор» — по ней фильтруются ресурсы поля.")
                color: theme.textMuted
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                visible: node.fileExchangeReady && node.fileSelectedShareRoot.length > 0
                spacing: 6
                IconButton {
                    theme: root.theme
                    glyph: "\uE74B"
                    enabled: node.fileBrowsePath.length > 0
                    onClicked: node.browseUp()
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: node.fileBrowseCrumbs
                        delegate: Label {
                            required property var modelData
                            required property int index
                            text: index > 0 ? " › " + modelData.label : modelData.label
                            color: index === node.fileBrowseCrumbs.length - 1
                                   ? theme.textPrimary : theme.accent
                            font.pixelSize: 11
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: node.browseToCrumb(index)
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: remoteList
                    anchors.fill: parent
                    clip: true
                    spacing: 4
                    visible: node.canFileList
                    model: node.remoteFiles
                    delegate: FileListRow {
                        theme: root.theme
                        node: root.node
                        name: model.name
                        hash: model.hash
                        sizeLabel: model.sizeLabel
                        mime: model.mime
                        isRemote: model.isRemote
                        isDirectory: model.isDirectory
                        navPath: model.navPath
                        rootPath: model.rootPath
                        NyxButtonSecondary {
                            visible: node.canFileDownload && !isDirectory
                            theme: root.theme
                            text: qsTr("Скачать")
                            onClicked: node.downloadFile(hash)
                        }
                        NyxButtonSecondary {
                            visible: node.canFileOpenRemote && !isDirectory
                            theme: root.theme
                            text: qsTr("Открыть")
                            onClicked: node.openRemoteFile(hash)
                        }
                    }
                }

                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: !node.canFileList && node.fileExchangeReady
                    theme: root.theme
                    emoji: "🔒"
                    title: qsTr("Нет доступа к списку")
                    hint: qsTr("Обратитесь к владельцу поля")
                }

                EmptyState {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: remoteList.count === 0 && node.fileExchangeReady && node.canFileList
                    theme: root.theme
                    emoji: "🌐"
                    title: qsTr("Список пуст")
                    hint: qsTr("Обновите список в шапке")
                }
            }
        }
    }

    // ===== Страница: Доступ (только для поля) =====
    Component {
        id: accessPage

        ScrollView {
            anchors.fill: parent
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent.width
                spacing: theme.spacing

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: node.canManageFileRoles
                          ? qsTr("Роли и участники поля — общие права на все share-папки. Новые папки добавляются в область, выбранную в шапке.")
                          : qsTr("Права на файлы назначает владелец поля.")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                Label {
                    text: qsTr("Участники")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: node.fileMemberAccess
                    delegate: Rectangle {
                        required property string userId
                        required property string nickname
                        required property string idShort
                        required property string roleId

                        Layout.fillWidth: true
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
                                text: nickname + " · " + idShort
                                color: theme.textPrimary
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            NyxComboBox {
                                Layout.preferredWidth: 150
                                theme: root.theme
                                enabled: node.canManageFileRoles
                                model: node.fileRoleList
                                textRole: "name"
                                Component.onCompleted: syncRole()
                                function syncRole() {
                                    const roles = node.fileRoleList
                                    for (let i = 0; i < roles.length; ++i) {
                                        if (roles[i].roleId === roleId) {
                                            currentIndex = i
                                            return
                                        }
                                    }
                                }
                                onActivated: {
                                    const roles = node.fileRoleList
                                    if (currentIndex >= 0 && currentIndex < roles.length)
                                        node.setMemberFileRole(userId, roles[currentIndex].roleId)
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.topMargin: 8
                    text: qsTr("Роли")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                }

                Repeater {
                    model: node.fileRoleList
                    delegate: Rectangle {
                        required property string roleId
                        required property string name
                        required property int permissions
                        required property bool builtin

                        Layout.fillWidth: true
                        implicitHeight: roleCol.implicitHeight + 16
                        radius: theme.radiusBtn
                        color: theme.btnSecondary
                        border.color: theme.border

                        ColumnLayout {
                            id: roleCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            spacing: 6
                            Label {
                                Layout.fillWidth: true
                                text: name + (builtin ? qsTr(" · встроенная") : "")
                                color: theme.textPrimary
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 6
                                Repeater {
                                    model: [
                                        { bit: node.permFileList, label: qsTr("Список") },
                                        { bit: node.permFileDownload, label: qsTr("Скачать") },
                                        { bit: node.permFileUpload, label: qsTr("Отправить") },
                                        { bit: node.permFileDelete, label: qsTr("Удалить") },
                                        { bit: node.permFileOpenRemote, label: qsTr("По сети") },
                                        { bit: node.permFileManageShares, label: qsTr("Папки") },
                                        { bit: node.permFileManageRoles, label: qsTr("Роли") }
                                    ]
                                    delegate: Rectangle {
                                        required property int bit
                                        required property string label
                                        radius: 10
                                        height: 22
                                        width: chipText.implicitWidth + 16
                                        color: (permissions & bit) ? theme.accent : theme.inputBg
                                        border.color: theme.border
                                        opacity: node.canManageFileRoles && !builtin ? 1.0 : 0.85
                                        Label {
                                            id: chipText
                                            anchors.centerIn: parent
                                            text: label
                                            color: (permissions & bit) ? theme.textPrimary : theme.textMuted
                                            font.pixelSize: 10
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: node.canManageFileRoles && !builtin
                                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                            onClicked: node.toggleFileRolePermission(roleId, bit)
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: node.canManageFileRoles && !builtin
                                Item { Layout.fillWidth: true }
                                NyxButtonSecondary {
                                    theme: root.theme
                                    text: qsTr("Удалить роль")
                                    onClicked: node.deleteFileRole(roleId)
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: node.canManageFileRoles
                    spacing: 8
                    NyxTextField {
                        id: newRoleName
                        Layout.fillWidth: true
                        theme: root.theme
                        placeholderText: qsTr("Имя новой роли")
                    }
                    NyxButtonSecondary {
                        theme: root.theme
                        text: qsTr("Создать")
                        enabled: newRoleName.text.trim().length > 0
                        onClicked: {
                            node.createFileRole(newRoleName.text,
                                                node.permFileList | node.permFileDownload)
                            newRoleName.clear()
                        }
                    }
                }

                Item { Layout.preferredHeight: theme.spacing }
            }
        }
    }
}
