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

        background: Rectangle {
            radius: theme.radiusInput
            color: theme.inputBg
            border.color: theme.border
            border.width: 1
        }
        contentItem: Text {
            leftPadding: 10
            text: modeBox.displayText
            color: theme.textPrimary
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
        }
        popup: Popup {
            y: modeBox.height
            width: modeBox.width
            padding: 4
            background: Rectangle {
                color: theme.bgSidebar
                border.color: theme.border
                radius: theme.radiusBtn
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: modeBox.delegateModel
                ScrollIndicator.vertical: ScrollIndicator { }
            }
        }
        delegate: ItemDelegate {
            width: modeBox.width - 8
            text: modelData
            highlighted: modeBox.highlightedIndex === index
            background: Rectangle {
                color: highlighted ? theme.btnSecondaryHover : "transparent"
                radius: theme.radiusBtn - 2
            }
            contentItem: Text {
                text: parent.text
                color: theme.textPrimary
                font.pixelSize: 13
                leftPadding: 8
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label {
            Layout.fillWidth: true
            text: qsTr("Автозапуск hub моих полей")
            color: theme.textPrimary
            wrapMode: Text.WordWrap
            font.pixelSize: 13
        }
        Switch {
            checked: node.autoStartOwnedHub
            onToggled: node.autoStartOwnedHub = checked
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("При создании поля и входе в аккаунт hub запускается автоматически (только для создателя).")
        color: theme.textMuted
        font.pixelSize: 11
    }

    Label {
        text: qsTr("Rendezvous-серверы (через запятую)")
        color: theme.textSecondary
        font.pixelSize: 11
    }

    NyxTextField {
        id: rendezvousListField
        Layout.fillWidth: true
        theme: root.theme
        text: node.rendezvousList
        placeholderText: "rv.example.com:3478,127.0.0.1:3478"
        font.family: "Consolas"
        font.pixelSize: 11
        onEditingFinished: {
            node.rendezvousList = text
            node.saveNetworkSettings()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        NyxButtonSecondary {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Проверить первый")
            onClicked: {
                const parts = rendezvousListField.text.split(",")
                if (parts.length > 0)
                    node.testRendezvousServer(parts[0].trim())
            }
        }
        NyxButton {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Сохранить")
            onClicked: {
                if (node.rendezvousList !== rendezvousListField.text)
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
