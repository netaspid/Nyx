import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "controls"
import "components"

/** Экран входа: локальные P2P-аккаунты, без серверной регистрации. */
Rectangle {
    id: root
    required property var theme
    required property var node

    anchors.fill: parent
    color: theme.bgApp
    visible: !node.sessionUnlocked
    z: 100

    property int mode: node.accountList.length > 0 ? 0 : 1
    property string selectedAccountId: ""
    property string selectedNickname: ""

    function avatarColor(name) {
        var palette = ["#5288c1", "#6ab2f2", "#7b68ee", "#5b9a8b", "#c27856", "#9b59b6"]
        var hash = 0
        for (var i = 0; i < name.length; ++i)
            hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0
        return palette[Math.abs(hash) % palette.length]
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 48)
        spacing: theme.spacing

        NyxLogo {
            Layout.alignment: Qt.AlignHCenter
            theme: root.theme
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Nyx — зашифрованный P2P-мессенджер")
            color: theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Ключи только на вашем устройстве. Rendezvous не видит сообщения и не хранит пароли.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        Label {
            visible: node.accountGateError.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: node.accountGateError
            color: "#e57373"
            font.pixelSize: 11
        }

        ColumnLayout {
            visible: root.mode === 0 && node.accountList.length > 0
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Ваши аккаунты")
                color: theme.textSecondary
                font.pixelSize: 12
                font.capitalization: Font.AllUppercase
            }

            ListView {
                id: accountListView
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(280, Math.max(56, accountListView.count * 58))
                spacing: 8
                clip: true
                model: node.accountList
                delegate: Rectangle {
                    required property var modelData
                    width: accountListView.width
                    height: 52
                    radius: theme.radiusInput
                    color: root.selectedAccountId === modelData.id ? theme.inputBg : "transparent"
                    border.color: root.selectedAccountId === modelData.id ? theme.accent : theme.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 10

                        AvatarBadge {
                            size: 36
                            label: modelData.nickname
                            baseColor: root.avatarColor(modelData.nickname)
                            textColor: theme.textPrimary
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: modelData.nickname
                                color: theme.textPrimary
                                font.pixelSize: 14
                            }
                            Label {
                                text: "id " + modelData.idShort + (modelData.locked ? " · занят" : "")
                                color: theme.textMuted
                                font.pixelSize: 10
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: !modelData.locked
                        onClicked: {
                            root.selectedAccountId = modelData.id
                            root.selectedNickname = modelData.nickname
                        }
                    }
                }
            }

            NyxTextField {
                id: unlockPassword
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Пароль")
                echoMode: TextInput.Password
                enabled: root.selectedAccountId.length > 0
            }

            NyxButton {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Войти")
                enabled: root.selectedAccountId.length > 0 && unlockPassword.text.length > 0
                onClicked: node.unlockAccount(root.selectedAccountId, unlockPassword.text)
            }

            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Создать новый аккаунт")
                onClicked: root.mode = 1
            }
        }

        ColumnLayout {
            visible: root.mode === 1 || node.accountList.length === 0
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Новый аккаунт")
                color: theme.textSecondary
                font.pixelSize: 12
                font.capitalization: Font.AllUppercase
            }

            NyxTextField {
                id: createNick
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Никнейм (виден собеседникам)")
            }

            NyxTextField {
                id: createPassword
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Пароль (мин. 8 символов)")
                echoMode: TextInput.Password
            }

            NyxTextField {
                id: createPassword2
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Повтор пароля")
                echoMode: TextInput.Password
            }

            NyxButton {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Создать и войти")
                enabled: createNick.text.trim().length > 0
                         && createPassword.text.length >= 8
                         && createPassword2.text.length >= 8
                onClicked: node.createAccount(createNick.text, createPassword.text, createPassword2.text)
            }

            NyxButtonSecondary {
                visible: node.accountList.length > 0
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Назад к списку")
                onClicked: root.mode = 0
            }
        }

        ColumnLayout {
            visible: node.legacyProfilePending
            Layout.fillWidth: true
            spacing: 8
            Layout.topMargin: 8

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: theme.border
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Найден старый профиль без шифрования. Задайте пароль для защиты ключей.")
                color: theme.textSecondary
                font.pixelSize: 11
            }

            NyxTextField {
                id: migratePassword
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Пароль для шифрования")
                echoMode: TextInput.Password
            }

            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Импортировать старый профиль")
                enabled: migratePassword.text.length >= 8
                onClicked: node.importLegacyProfile(migratePassword.text)
            }
        }
    }
}
