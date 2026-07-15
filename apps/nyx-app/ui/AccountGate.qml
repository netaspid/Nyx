import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "controls"
import "components"

/** Экран входа: локальные аккаунты, recovery, remember-me. */
Rectangle {
    id: root
    required property var theme
    required property var node

    anchors.fill: parent
    color: theme.bgApp
    visible: !node.sessionUnlocked || node.needsRecoveryConfirm
    z: 100

    /** 0 — список, 1 — создать, 2 — показать recovery, 3 — сброс пароля */
    property int mode: node.needsRecoveryConfirm ? 2
                       : (node.accountList.length > 0 ? 0 : 1)
    property string selectedAccountId: ""
    property string selectedNickname: ""
    property string localHint: ""

    Component.onCompleted: preselectLastAccount()
    Connections {
        target: node
        function onAccountGateChanged() {
            if (node.needsRecoveryConfirm) root.mode = 2
            root.preselectLastAccount()
        }
    }

    function preselectLastAccount() {
        if (root.selectedAccountId.length > 0) return
        const last = node.lastAccountId || ""
        if (last.length === 0) {
            // Автовыбор единственного доступного аккаунта
            if (node.accountList.length === 1 && !node.accountList[0].locked) {
                root.selectedAccountId = node.accountList[0].id
                root.selectedNickname = node.accountList[0].nickname
            }
            return
        }
        for (let i = 0; i < node.accountList.length; ++i) {
            const a = node.accountList[i]
            if (a.id === last && !a.locked) {
                root.selectedAccountId = a.id
                root.selectedNickname = a.nickname
                return
            }
        }
    }

    function openPasswordReset() {
        root.localHint = ""
        if (root.selectedAccountId.length === 0) {
            root.localHint = qsTr("Сначала выберите аккаунт")
            return
        }
        if (!root.selectedHasRecovery()) {
            root.localHint = qsTr("У этого аккаунта нет recovery-фразы (создан до обновления). Сброс невозможен — войдите паролем или создайте новый аккаунт.")
            return
        }
        root.mode = 3
    }

    function avatarColor(name) {
        var palette = ["#5288c1", "#6ab2f2", "#7b68ee", "#5b9a8b", "#c27856", "#9b59b6"]
        var hash = 0
        for (var i = 0; i < name.length; ++i)
            hash = ((hash << 5) - hash + name.charCodeAt(i)) | 0
        return palette[Math.abs(hash) % palette.length]
    }

    function selectedHasRecovery() {
        for (let i = 0; i < node.accountList.length; ++i) {
            if (node.accountList[i].id === root.selectedAccountId)
                return node.accountList[i].hasRecovery === true
        }
        return false
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 48)
        spacing: theme.spacing

        NyxLogo {
            Layout.alignment: Qt.AlignHCenter
            theme: root.theme
            large: true
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
            text: qsTr("Ключи только на вашем устройстве. Пароль и recovery-фраза никуда не отправляются.")
            color: theme.textMuted
            font.pixelSize: 11
        }

        Label {
            visible: node.accountGateError.length > 0 || root.localHint.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: node.accountGateError.length > 0 ? node.accountGateError : root.localHint
            color: "#e57373"
            font.pixelSize: 11
        }

        // --- Login ---
        ColumnLayout {
            visible: root.mode === 0 && node.accountList.length > 0 && !node.needsRecoveryConfirm
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
                Layout.preferredHeight: Math.min(220, Math.max(56, accountListView.count * 58))
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
                                text: {
                                    var t = "id " + modelData.idShort
                                    if (modelData.locked) t += qsTr(" · занят")
                                    else if (modelData.rememberActive) t += qsTr(" · сохранён вход")
                                    return t
                                }
                                color: theme.textMuted
                                font.pixelSize: 10
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: !modelData.locked
                        onClicked: {
                            root.localHint = ""
                            root.selectedAccountId = modelData.id
                            root.selectedNickname = modelData.nickname
                        }
                        onDoubleClicked: {
                            if (modelData.rememberActive)
                                node.tryUnlockRemembered(modelData.id)
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
                Keys.onReturnPressed: unlockBtn.clicked()
            }

            NyxCheckBox {
                id: rememberUnlock
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Запомнить меня на 30 дней")
                checked: true
                enabled: root.selectedAccountId.length > 0
            }

            NyxButton {
                id: unlockBtn
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Войти")
                enabled: root.selectedAccountId.length > 0 && unlockPassword.text.length > 0
                onClicked: node.unlockAccount(root.selectedAccountId, unlockPassword.text,
                                              rememberUnlock.checked)
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Забыли пароль?")
                color: theme.accent
                font.pixelSize: 12
                font.underline: true
                opacity: root.selectedAccountId.length > 0 ? 1 : 0.4
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openPasswordReset()
                }
            }

            NyxButtonSecondary {
                readonly property bool canRememberLogin: {
                    for (let i = 0; i < node.accountList.length; ++i) {
                        if (node.accountList[i].id === root.selectedAccountId)
                            return node.accountList[i].rememberActive === true
                                    && !node.accountList[i].locked
                    }
                    return false
                }
                visible: canRememberLogin
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Войти без пароля")
                onClicked: node.tryUnlockRemembered(root.selectedAccountId)
            }

            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Создать новый аккаунт")
                onClicked: {
                    root.localHint = ""
                    root.mode = 1
                }
            }
        }

        // --- Create ---
        ColumnLayout {
            visible: (root.mode === 1 || node.accountList.length === 0) && !node.needsRecoveryConfirm
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

            NyxCheckBox {
                id: rememberCreate
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Запомнить меня на 30 дней")
                checked: true
            }

            NyxButton {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Создать")
                enabled: createNick.text.trim().length > 0
                         && createPassword.text.length >= 8
                         && createPassword2.text.length >= 8
                onClicked: node.createAccount(createNick.text, createPassword.text,
                                              createPassword2.text, rememberCreate.checked)
            }

            NyxButtonSecondary {
                visible: node.accountList.length > 0
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Назад к списку")
                onClicked: root.mode = 0
            }
        }

        // --- Recovery confirm ---
        ColumnLayout {
            visible: root.mode === 2 || node.needsRecoveryConfirm
            Layout.fillWidth: true
            spacing: 10

            Label {
                text: qsTr("Сохраните recovery-фразу")
                color: theme.textSecondary
                font.pixelSize: 12
                font.capitalization: Font.AllUppercase
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Это единственный способ сбросить пароль. Запишите 12 слов и храните офлайн. Без фразы ключи аккаунта восстановить нельзя.")
                color: theme.textMuted
                font.pixelSize: 11
            }

            Rectangle {
                Layout.fillWidth: true
                radius: theme.radiusInput
                color: theme.inputBg
                border.color: theme.accent
                implicitHeight: phraseLabel.implicitHeight + 24

                Label {
                    id: phraseLabel
                    anchors.fill: parent
                    anchors.margins: 12
                    wrapMode: Text.WordWrap
                    text: node.pendingRecoveryPhrase
                    color: theme.textPrimary
                    font.pixelSize: 14
                    font.family: "Consolas"
                }
            }

            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Скопировать фразу")
                onClicked: node.copyRecoveryPhrase()
            }

            NyxCheckBox {
                id: phraseSaved
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Я сохранил recovery-фразу в надёжном месте")
            }

            NyxButton {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Продолжить")
                enabled: phraseSaved.checked && node.pendingRecoveryPhrase.length > 0
                onClicked: node.confirmRecoveryPhraseSaved()
            }
        }

        // --- Reset password ---
        ColumnLayout {
            visible: root.mode === 3 && !node.needsRecoveryConfirm
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Сброс пароля")
                color: theme.textSecondary
                font.pixelSize: 12
                font.capitalization: Font.AllUppercase
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Аккаунт: %1").arg(root.selectedNickname.length
                                              ? root.selectedNickname
                                              : root.selectedAccountId.substring(0, 8))
                color: theme.textPrimary
                font.pixelSize: 13
            }

            NyxTextField {
                id: resetPhrase
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Recovery-фраза (12 слов)")
            }

            NyxTextField {
                id: resetPassword
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Новый пароль (мин. 8)")
                echoMode: TextInput.Password
            }

            NyxTextField {
                id: resetPassword2
                Layout.fillWidth: true
                theme: root.theme
                placeholderText: qsTr("Повтор нового пароля")
                echoMode: TextInput.Password
            }

            NyxButton {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Сбросить пароль")
                enabled: root.selectedAccountId.length > 0
                         && resetPhrase.text.trim().length > 0
                         && resetPassword.text.length >= 8
                         && resetPassword2.text.length >= 8
                onClicked: {
                    if (node.resetPasswordWithRecovery(root.selectedAccountId, resetPhrase.text,
                                                       resetPassword.text, resetPassword2.text)) {
                        resetPhrase.text = ""
                        resetPassword.text = ""
                        resetPassword2.text = ""
                        unlockPassword.text = ""
                        root.mode = 0
                    }
                }
            }

            NyxButtonSecondary {
                Layout.fillWidth: true
                theme: root.theme
                text: qsTr("Назад")
                onClicked: root.mode = 0
            }
        }

        ColumnLayout {
            visible: node.legacyProfilePending && !node.needsRecoveryConfirm
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
