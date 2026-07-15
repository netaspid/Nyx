import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

/** Диалог прав: роль на объект и переопределения для участников. */
Popup {
    id: root
    required property var theme
    required property var node

    /** "path" — папка/файл; "field" — роли участников поля. */
    property string mode: "path"
    property string objectTitle: ""
    property int grantSyncKey: 0

    parent: Overlay.overlay

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: theme.spacing
    width: Math.min(540, parent ? parent.width - 48 : 540)
    implicitHeight: contentCol.implicitHeight + padding * 2
    anchors.centerIn: Overlay.overlay

    function openForPath(rootPath, relativePath, title) {
        mode = "path"
        objectTitle = title.length > 0 ? qsTr("Права: %1").arg(title) : qsTr("Права доступа")
        node.setFileAccessTarget(rootPath, relativePath)
        open()
    }

    /** Права на share-корень (относительный путь пустой). */
    function openForShareRoot(rootPath, title) {
        openForPath(rootPath, "", title)
    }

    function openForField() {
        mode = "field"
        objectTitle = qsTr("Участники поля")
        node.refreshFieldRoster()
        open()
    }

    onOpened: {
        pathRoleModel.rebuild()
        pathGrantModel.rebuild()
        fieldRoleModel.rebuild()
        grantSyncKey++
        Qt.callLater(function() {
            pathRoleCombo.syncPathRole()
        })
    }

    background: Rectangle {
        radius: theme.radiusBtn
        color: theme.bgSidebar
        border.color: theme.border
    }

    contentItem: ColumnLayout {
        id: contentCol
        spacing: theme.spacing
        width: root.availableWidth

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: objectTitle
                color: theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
            }
            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: theme.radiusBtn
                color: accessCloseArea.containsMouse ? (theme.darkMode ? "#c42b1c" : "#e81123")
                                                     : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "\uE8BB"
                    font.family: "Segoe MDL2 Assets"
                    font.pixelSize: 12
                    color: accessCloseArea.containsMouse ? "#ffffff" : theme.textSecondary
                }
                MouseArea {
                    id: accessCloseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }

        // --- Роль на объект (не требует участников) ---
        Rectangle {
            Layout.fillWidth: true
            visible: mode === "path"
            implicitHeight: pathRoleCol.implicitHeight + 16
            radius: theme.radiusBtn
            color: theme.btnSecondary
            border.color: theme.border

            ColumnLayout {
                id: pathRoleCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Роль на этот объект")
                    color: theme.textPrimary
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Применяется ко всем участникам, если для них нет отдельного переопределения ниже.")
                    color: theme.textMuted
                    font.pixelSize: 10
                }
                NyxComboBox {
                    id: pathRoleCombo
                    Layout.fillWidth: true
                    theme: root.theme
                    enabled: node.canManageFileRoles
                    model: pathRoleModel
                    textRole: "label"
                    Component.onCompleted: syncPathRole()
                    function syncPathRole() {
                        if (node.filePathRoleId.length === 0) {
                            currentIndex = 0
                            return
                        }
                        for (let i = 1; i < pathRoleModel.count; ++i) {
                            if (pathRoleModel.get(i).roleId === node.filePathRoleId) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = 0
                    }
                    onActivated: {
                        const item = pathRoleModel.get(currentIndex)
                        if (item.mode === "inherit")
                            node.clearPathRole()
                        else
                            node.setPathRole(item.roleId)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: node.filePathRoleInheritedFrom.length > 0
                              && node.filePathRoleId.length === 0
                    text: qsTr("Наследует роль от: %1").arg(node.filePathRoleInheritedFrom)
                    color: theme.accent
                    font.pixelSize: 10
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: mode === "path"
            text: qsTr("Переопределения для участников")
            color: theme.textSecondary
            font.pixelSize: 11
            font.capitalization: Font.AllUppercase
        }

        Label {
            Layout.fillWidth: true
            visible: mode === "field"
            wrapMode: Text.WordWrap
            text: qsTr("Роль по умолчанию для всего поля.")
            color: theme.textMuted
            font.pixelSize: 10
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(280, Math.max(60, memberColumn.implicitHeight + 8))
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                id: memberColumn
                width: root.availableWidth
                spacing: 8

                Label {
                    width: parent.width
                    visible: memberRepeater.count === 0
                    wrapMode: Text.WordWrap
                    text: mode === "field"
                          ? qsTr("Участники появятся после входа в поле. Состав сохраняется локально.")
                          : qsTr("Нет других участников — достаточно роли объекта выше.")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                Repeater {
                    id: memberRepeater
                    model: {
                        const all = mode === "field" ? node.fileMemberAccess : node.filePathMemberAccess
                        const out = []
                        for (let i = 0; i < all.length; ++i) {
                            if (!all[i].isOwner) out.push(all[i])
                        }
                        return out
                    }
                    delegate: Rectangle {
                        required property string userId
                        required property string nickname
                        required property string idShort
                        required property string roleId
                        required property string grantMode
                        required property int directPermissions
                        required property string inheritedFrom

                        width: memberColumn.width
                        implicitHeight: inner.implicitHeight + 16
                        radius: theme.radiusBtn
                        color: theme.btnSecondary
                        border.color: theme.border

                        ColumnLayout {
                            id: inner
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 8
                            spacing: 6
                            width: parent.width

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: nickname + " · " + idShort
                                    color: theme.textPrimary
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                NyxComboBox {
                                    id: pathGrantCombo
                                    Layout.preferredWidth: 170
                                    theme: root.theme
                                    enabled: node.canManageFileRoles
                                    model: mode === "field" ? fieldRoleModel : pathGrantModel
                                    textRole: "label"
                                    property int syncKey: root.grantSyncKey
                                    onSyncKeyChanged: syncGrant()
                                    Component.onCompleted: syncGrant()
                                    function syncGrant() {
                                        if (mode === "field") {
                                            for (let i = 0; i < fieldRoleModel.count; ++i) {
                                                if (fieldRoleModel.get(i).roleId === roleId) {
                                                    currentIndex = i
                                                    return
                                                }
                                            }
                                            currentIndex = fieldRoleModel.count > 0 ? 0 : -1
                                            return
                                        }
                                        if (grantMode === "direct") {
                                            currentIndex = pathGrantModel.count - 1
                                            return
                                        }
                                        if (grantMode === "role" && roleId.length > 0) {
                                            for (let i = 1; i < pathGrantModel.count - 1; ++i) {
                                                if (pathGrantModel.get(i).roleId === roleId) {
                                                    currentIndex = i
                                                    return
                                                }
                                            }
                                        }
                                        currentIndex = 0
                                    }
                                    onActivated: {
                                        if (mode === "field") {
                                            const item = fieldRoleModel.get(currentIndex)
                                            if (item) node.setMemberFileRole(userId, item.roleId)
                                            return
                                        }
                                        const item = pathGrantModel.get(currentIndex)
                                        if (item.mode === "inherit")
                                            node.clearPathMemberGrant(userId)
                                        else if (item.mode === "direct")
                                            node.setPathGrantDirect(userId)
                                        else
                                            node.setPathMemberFileRole(userId, item.roleId)
                                    }
                                    Connections {
                                        target: node
                                        function onFileAccessChanged() {
                                            Qt.callLater(pathGrantCombo.syncGrant)
                                        }
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: mode === "path" && grantMode === "inherited"
                                         && inheritedFrom.length > 0
                                text: qsTr("Наследует: %1").arg(inheritedFrom)
                                color: theme.accent
                                font.pixelSize: 10
                            }
                            Flow {
                                Layout.fillWidth: true
                                visible: mode === "path" && grantMode === "direct"
                                spacing: 6
                                Repeater {
                                    model: [
                                        { bit: node.permFileList, label: qsTr("Список") },
                                        { bit: node.permFileDownload, label: qsTr("Скачать") },
                                        { bit: node.permFileUpload, label: qsTr("Отправить") },
                                        { bit: node.permFileOpenRemote, label: qsTr("По сети") }
                                    ]
                                    delegate: Rectangle {
                                        required property int bit
                                        required property string label
                                        radius: 10
                                        height: 22
                                        width: pChip.implicitWidth + 16
                                        color: (directPermissions & bit) ? theme.accent : theme.inputBg
                                        border.color: theme.border
                                        Label {
                                            id: pChip
                                            anchors.centerIn: parent
                                            text: label
                                            font.pixelSize: 10
                                            color: (directPermissions & bit) ? theme.textPrimary : theme.textMuted
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: node.canManageFileRoles
                                            onClicked: node.togglePathDirectPermission(userId, bit)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

    }

    ListModel {
        id: pathRoleModel
        function rebuild() {
            clear()
            append({ label: qsTr("По умолчанию (поле / родитель)"), mode: "inherit", roleId: "" })
            const roles = node.fileRoleList
            for (let i = 0; i < roles.length; ++i) {
                if (roles[i].roleId === "owner") continue
                append({ label: roles[i].name, mode: "role", roleId: roles[i].roleId })
            }
        }
    }

    ListModel {
        id: pathGrantModel
        function rebuild() {
            clear()
            append({ label: qsTr("По умолчанию"), mode: "inherit", roleId: "" })
            const roles = node.fileRoleList
            for (let i = 0; i < roles.length; ++i) {
                if (roles[i].roleId === "owner") continue
                append({ label: roles[i].name, mode: "role", roleId: roles[i].roleId })
            }
            append({ label: qsTr("Прямые права…"), mode: "direct", roleId: "" })
        }
    }

    ListModel {
        id: fieldRoleModel
        function rebuild() {
            clear()
            const roles = node.fileRoleList
            for (let i = 0; i < roles.length; ++i) {
                if (roles[i].roleId === "owner") continue
                append({ label: roles[i].name, roleId: roles[i].roleId })
            }
        }
    }

    Connections {
        target: node
        function onFileAccessChanged() {
            pathRoleModel.rebuild()
            pathGrantModel.rebuild()
            fieldRoleModel.rebuild()
            grantSyncKey++
            Qt.callLater(function() { pathRoleCombo.syncPathRole() })
        }
    }

    Component.onCompleted: {
        pathRoleModel.rebuild()
        pathGrantModel.rebuild()
        fieldRoleModel.rebuild()
    }
}
