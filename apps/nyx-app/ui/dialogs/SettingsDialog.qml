import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"
import "."

Dialog {
    id: root
    required property var theme
    required property var node

    readonly property bool fullBleed: Qt.platform.os === "android"
                                      || (parent && parent.width < 720)

    modal: true
    standardButtons: Dialog.NoButton
    width: fullBleed ? (parent ? parent.width : Overlay.overlay.width) : (Math.min(440, parent ? parent.width - 48 : 440))
    height: fullBleed ? (parent ? parent.height : Overlay.overlay.height) : (Math.min(640, parent ? parent.height - 80 : 640))
    padding: 0
    x: fullBleed ? 0 : (parent ? Math.round((parent.width - width) / 2) : 0)
    y: fullBleed ? 0 : (parent ? Math.round((parent.height - height) / 2) : 0)

    background: Rectangle {
        color: theme.bgSidebar
        radius: root.fullBleed ? 0 : theme.radiusBtn
        border.color: theme.border
    }

    header: DialogChrome {
        theme: root.theme
        title: qsTr("Настройки")
        dialog: root
    }

    onOpened: node.refreshMediaDevices()

    contentItem: ColumnLayout {
        width: parent ? parent.width : implicitWidth
        spacing: 0

        Flickable {
            id: settingsFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: theme.spacing
            Layout.rightMargin: theme.spacing
            Layout.bottomMargin: theme.spacing
            clip: true
            readonly property int scrollGutter: contentHeight > height + 1 ? 14 : 0
            contentWidth: width
            contentHeight: settingsCol.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            interactive: contentHeight > height

            ScrollBar.vertical: ScrollBar {
                policy: settingsFlick.contentHeight > settingsFlick.height
                        ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                width: 10
                padding: 1
            }

            ColumnLayout {
                id: settingsCol
                width: settingsFlick.width - settingsFlick.scrollGutter
                spacing: theme.spacing

                Label {
                    text: qsTr("Профиль")
                    color: theme.textSecondary
                    font.pixelSize: 12
                    font.capitalization: Font.AllUppercase
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    AvatarBadge {
                        size: 64
                        label: node.profileNickname
                        baseColor: "#5288c1"
                        textColor: "#ffffff"
                        imageSource: node.profileAvatarPath
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        NyxButton {
                            Layout.fillWidth: true
                            theme: root.theme
                            text: qsTr("Выбрать фото")
                            onClicked: node.pickAndSetProfilePhoto()
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("До 5 фото в истории. Peer видит их после Hello и подтягивает по P2P.")
                            color: theme.textMuted
                            font.pixelSize: 11
                        }
                    }
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.preferredHeight: node.profilePhotoList.length > 0 ? 72 : 0
                    visible: node.profilePhotoList.length > 0
                    contentWidth: photoRow.implicitWidth
                    clip: true
                    flickableDirection: Flickable.HorizontalFlick
                    Row {
                        id: photoRow
                        spacing: 8
                        Repeater {
                            model: node.profilePhotoList
                            delegate: Item {
                                required property var modelData
                                width: 64
                                height: 64
                                AvatarBadge {
                                    anchors.fill: parent
                                    size: 64
                                    label: node.profileNickname
                                    baseColor: "#5288c1"
                                    imageSource: modelData.path || ""
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: function(mouse) {
                                        if (mouse.button === Qt.RightButton)
                                            node.removeProfilePhoto(modelData.hash)
                                        else
                                            node.makeProfilePhotoCurrent(modelData.hash)
                                    }
                                    ToolTip.visible: containsMouse
                                    ToolTip.text: qsTr("ЛКМ — сделать текущим · ПКМ — удалить")
                                    hoverEnabled: true
                                }
                            }
                        }
                    }
                }

                NyxTextField {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: node.profileNickname
                    placeholderText: qsTr("Никнейм")
                    onEditingFinished: node.profileNickname = text
                }

                NyxTextField {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: node.profileBio
                    placeholderText: qsTr("Подпись о себе")
                    onEditingFinished: node.profileBio = text
                }

                NyxTextField {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: node.profileInterests
                    placeholderText: qsTr("Интересы через запятую")
                    onEditingFinished: node.profileInterests = text
                }

                Label {
                    text: qsTr("Доступность")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                NyxComboBox {
                    id: availBox
                    Layout.fillWidth: true
                    theme: root.theme
                    textRole: "text"
                    model: [
                        { value: "available", text: qsTr("Доступен") },
                        { value: "away", text: qsTr("Отошёл") },
                        { value: "busy", text: qsTr("Занят") },
                        { value: "invisible", text: qsTr("Невидимый") }
                    ]
                    Component.onCompleted: syncAvail()
                    onCurrentIndexChanged: {
                        const row = rowAt(currentIndex)
                        if (row && row.value)
                            node.profileAvailability = row.value
                    }
                    function syncAvail() {
                        const cur = node.profileAvailability
                        for (let i = 0; i < model.length; ++i) {
                            if (model[i].value === cur) {
                                currentIndex = i
                                return
                            }
                        }
                    }
                    Connections {
                        target: node
                        function onProfileMetaChanged() { availBox.syncAvail() }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Подпись и интересы уходят peer при подключении (Hello). Хранятся только локально у вас и у них. Изменения сохраняются сами — без отдельной кнопки.")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                NyxButton {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Копировать id")
                    onClicked: node.copyToClipboard(node.profileIdShort)
                }

                Label {
                    text: qsTr("Звонки")
                    color: theme.textSecondary
                    font.pixelSize: 12
                    font.capitalization: Font.AllUppercase
                    Layout.topMargin: 8
                }

                Label {
                    visible: Qt.platform.os !== "android"
                    Layout.fillWidth: true
                    text: qsTr("Камера")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                NyxComboBox {
                    id: cameraBox
                    visible: Qt.platform.os !== "android"
                    Layout.fillWidth: true
                    theme: root.theme
                    textRole: "text"
                    model: node.cameraDeviceList
                    Component.onCompleted: syncCamera()
                    onActivated: {
                        const row = rowAt(currentIndex)
                        if (row && row.id !== undefined)
                            node.selectedCameraId = row.id
                    }
                    function syncCamera() {
                        const cur = node.selectedCameraId
                        const list = node.cameraDeviceList
                        for (let i = 0; i < list.length; ++i) {
                            if (list[i].id === cur) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = list.length > 0 ? 0 : -1
                    }
                    Connections {
                        target: node
                        function onMediaDevicesChanged() { cameraBox.syncCamera() }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Микрофон")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                NyxComboBox {
                    id: micBox
                    Layout.fillWidth: true
                    theme: root.theme
                    textRole: "text"
                    model: node.audioInputDeviceList
                    Component.onCompleted: syncMic()
                    onActivated: {
                        const row = rowAt(currentIndex)
                        if (row && row.id !== undefined)
                            node.selectedAudioInputId = row.id
                    }
                    function syncMic() {
                        const cur = node.selectedAudioInputId
                        const list = node.audioInputDeviceList
                        for (let i = 0; i < list.length; ++i) {
                            if (list[i].id === cur) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = list.length > 0 ? 0 : -1
                    }
                    Connections {
                        target: node
                        function onMediaDevicesChanged() { micBox.syncMic() }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Динамик / выход")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                NyxComboBox {
                    id: speakerBox
                    Layout.fillWidth: true
                    theme: root.theme
                    textRole: "text"
                    model: node.audioOutputDeviceList
                    Component.onCompleted: syncSpeaker()
                    onActivated: {
                        const row = rowAt(currentIndex)
                        if (row && row.id !== undefined)
                            node.selectedAudioOutputId = row.id
                    }
                    function syncSpeaker() {
                        const cur = node.selectedAudioOutputId
                        const list = node.audioOutputDeviceList
                        for (let i = 0; i < list.length; ++i) {
                            if (list[i].id === cur) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = list.length > 0 ? 0 : -1
                    }
                    Connections {
                        target: node
                        function onMediaDevicesChanged() { speakerBox.syncSpeaker() }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: Qt.platform.os === "android"
                          ? qsTr("Камеру переключайте во время видеозвонка. Устройства звука применяются сразу.")
                          : qsTr("Выбор сохраняется и применяется в следующих звонках (и сразу, если звонок уже идёт).")
                    color: theme.textMuted
                    font.pixelSize: 11
                }

                Label {
                    text: qsTr("Оформление")
                    color: theme.textSecondary
                    font.pixelSize: 12
                    font.capitalization: Font.AllUppercase
                    Layout.topMargin: 8
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Тёмная тема")
                        color: theme.textPrimary
                    }
                    Switch {
                        checked: theme.darkMode
                        onToggled: theme.setDarkMode(checked)
                    }
                }

                Label {
                    text: qsTr("Сеть")
                    color: theme.textSecondary
                    font.pixelSize: 12
                    font.capitalization: Font.AllUppercase
                    Layout.topMargin: 8
                }

                NetworkSettingsSection {
                    Layout.fillWidth: true
                    theme: root.theme
                    node: root.node
                }

                NyxButtonSecondary {
                    Layout.fillWidth: true
                    theme: root.theme
                    text: qsTr("Выйти из аккаунта")
                    onClicked: {
                        root.close()
                        node.signOut()
                    }
                }

                Label {
                    text: qsTr("Папки для обмена — вкладка «Файлы» · Ctrl+Enter — отправить · Ctrl+K — сеть")
                    color: theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                } // settingsCol
        } // Flickable
    }

    footer: Item { implicitHeight: 4 }
}
