import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "."

Dialog {
    id: root
    required property var theme
    required property var node

    title: qsTr("Настройки")
    modal: true
    standardButtons: Dialog.Close
    width: Math.min(440, parent ? parent.width - 48 : 440)
    padding: theme.spacing

    background: Rectangle {
        color: theme.bgSidebar
        radius: theme.radiusBtn
        border.color: theme.border
    }

    ColumnLayout {
        spacing: theme.spacing
        width: parent.width

        Label {
            text: qsTr("Профиль")
            color: theme.textSecondary
            font.pixelSize: 12
            font.capitalization: Font.AllUppercase
        }

        NyxTextField {
            Layout.fillWidth: true
            theme: root.theme
            text: node.profileNickname
            placeholderText: qsTr("Никнейм")
            onEditingFinished: node.profileNickname = text
        }

        NyxButton {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Копировать id")
            onClicked: node.copyToClipboard(node.profileIdShort)
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
            text: qsTr("Папки — индексируйте через вкладку «Файлы» в панели подключения")
            color: theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            text: qsTr("Ctrl+Enter — отправить · Ctrl+K — connect · Esc — отключиться")
            color: theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
