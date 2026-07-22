import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Тост снизу справа (Overlay): единый стиль с приложением. */
Item {
    id: root
    required property var theme
    property string message: ""
    property bool isError: false
    property var clearFn: null

    readonly property bool hasMessage: message.length > 0
    readonly property int toastMaxWidth: 400

    // Якорь родителя (Overlay) задаёт позицию; здесь только размер карточки.
    width: Math.min(toastMaxWidth, Math.max(260, (parent ? parent.width : 400) - 40))
    height: hasMessage ? card.implicitHeight : 0
    visible: hasMessage
    opacity: hasMessage ? 1 : 0
    z: 1000

    Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

    Rectangle {
        id: card
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        implicitHeight: contentCol.implicitHeight + 28
        radius: 12
        color: isError
               ? (theme.darkMode ? "#3a2428" : "#fff5f5")
               : (theme.darkMode ? "#243040" : "#ffffff")
        border.width: 1
        border.color: isError
                      ? (theme.darkMode ? "#c62828" : "#ef9a9a")
                      : theme.border

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 4
            radius: 2
            color: isError ? "#e57373" : theme.accent
        }

        ColumnLayout {
            id: contentCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 18
            anchors.rightMargin: 14
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    radius: 14
                    color: isError
                           ? (theme.darkMode ? "#5c2b2b" : "#ffebee")
                           : (theme.darkMode ? "#2b5278" : "#e3f2fd")

                    NyxIcon {
                        anchors.centerIn: parent
                        name: isError ? "info" : "check"
                        width: 14
                        height: 14
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: isError ? qsTr("Ошибка") : qsTr("Уведомление")
                    color: theme.textSecondary
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                }

                MouseArea {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof root.clearFn === "function")
                            root.clearFn()
                    }
                    NyxIcon {
                        anchors.centerIn: parent
                        name: "close"
                        width: 12
                        height: 12
                        opacity: 0.7
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.message
                color: theme.textPrimary
                font.pixelSize: Math.round(14 * theme.fontScale)
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                lineHeight: 1.3
            }
        }
    }

    Timer {
        id: hideTimer
        interval: isError ? 5200 : 3800
        running: root.hasMessage
        onTriggered: {
            if (typeof root.clearFn === "function")
                root.clearFn()
        }
    }

    onMessageChanged: {
        if (hasMessage)
            hideTimer.restart()
    }
}
