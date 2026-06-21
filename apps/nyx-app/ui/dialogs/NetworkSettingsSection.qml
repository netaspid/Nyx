import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

/** Настройки обнаружения: rendezvous, режим LAN/Интернет. */
ColumnLayout {
    id: root
    required property var theme
    required property var node

    spacing: theme.spacing
    Layout.fillWidth: true

    Label {
        text: qsTr("Обнаружение peer")
        color: theme.textSecondary
        font.pixelSize: 12
        font.capitalization: Font.AllUppercase
    }

    Label {
        text: qsTr("Rendezvous нужен для связи через интернет (NAT). LAN работает без него.")
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        color: theme.textMuted
        font.pixelSize: 11
    }

    ComboBox {
        id: modeBox
        Layout.fillWidth: true
        model: [qsTr("Авто (LAN + Интернет)"), qsTr("Только LAN"), qsTr("Только Интернет")]
        currentIndex: node.discoveryMode
        onActivated: node.discoveryMode = currentIndex
    }

    Label {
        text: qsTr("Rendezvous-серверы (через запятую)")
        color: theme.textSecondary
        font.pixelSize: 11
    }

    NyxTextField {
        id: rendezvousListField
        Layout.fillWidth: true
        theme: theme
        text: node.rendezvousList
        placeholderText: "rv.example.com:3478,127.0.0.1:3478"
        font.family: "Consolas"
        font.pixelSize: 11
        onEditingFinished: node.rendezvousList = text
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        NyxButtonSecondary {
            Layout.fillWidth: true
            theme: theme
            text: qsTr("Проверить первый")
            onClicked: {
                const parts = rendezvousListField.text.split(",")
                if (parts.length > 0)
                    node.testRendezvousServer(parts[0].trim())
            }
        }
        NyxButton {
            Layout.fillWidth: true
            theme: theme
            text: qsTr("Сохранить")
            onClicked: {
                node.rendezvousList = rendezvousListField.text
                node.saveNetworkSettings()
            }
        }
    }

    Label {
        visible: node.networkStatus.length > 0
        text: node.networkStatus
        color: theme.accent
        font.pixelSize: 11
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
}
