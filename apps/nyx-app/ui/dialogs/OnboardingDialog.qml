import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../components"

Dialog {
    id: root
    required property var theme
    required property var node

    title: qsTr("Добро пожаловать в Nyx")
    modal: true
    anchors.centerIn: parent
    width: Math.min(400, parent ? parent.width - 48 : 400)
    visible: node.needsOnboarding
    onVisibleChanged: if (visible) open()

    Component.onCompleted: Qt.callLater(function() {
        if (root.node && root.node.needsOnboarding)
            root.open()
    })

    background: Rectangle {
        color: theme.bgSidebar
        radius: theme.radiusBtn
        border.color: theme.border
    }

    ColumnLayout {
        spacing: theme.spacing
        width: parent.width

        NyxLogo {
            Layout.alignment: Qt.AlignHCenter
            theme: root.theme
            large: true
        }

        Label {
            text: qsTr("Выберите отображаемое имя. Ключи будут созданы автоматически.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            color: theme.textSecondary
        }

        NyxTextField {
            id: nickField
            Layout.fillWidth: true
            theme: root.theme
            placeholderText: qsTr("Никнейм")
            text: node.profileNickname
        }

        NyxButton {
            Layout.fillWidth: true
            theme: root.theme
            text: qsTr("Начать")
            enabled: nickField.text.trim().length > 0
            onClicked: {
                node.completeOnboarding(nickField.text)
                root.close()
            }
        }
    }
}
