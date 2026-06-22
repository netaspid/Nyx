import QtQuick
import QtQuick.Controls

ComboBox {
    id: ctrl
    required property var theme

    implicitHeight: 36

    background: Rectangle {
        radius: theme.radiusInput
        color: theme.inputBg
        border.color: ctrl.activeFocus ? theme.focusRing : theme.border
        border.width: ctrl.activeFocus ? 2 : 1
    }

    contentItem: Text {
        leftPadding: 10
        rightPadding: ctrl.indicator.width + 8
        text: ctrl.displayText
        color: theme.textPrimary
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: ctrl.width - width - 10
        y: (ctrl.height - height) / 2
        text: "\uE70D"
        font.family: "Segoe MDL2 Assets"
        font.pixelSize: 10
        color: theme.textSecondary
    }

    popup: Popup {
        y: ctrl.height + 2
        width: ctrl.width
        padding: 4

        background: Rectangle {
            radius: theme.radiusBtn
            color: theme.bgSidebar
            border.color: theme.border
        }

        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 240)
            model: ctrl.delegateModel
            delegate: ItemDelegate {
                width: ctrl.width - 8
                text: model[ctrl.textRole]
                highlighted: ctrl.highlightedIndex === index
                padding: 8

                background: Rectangle {
                    radius: theme.radiusBtn - 2
                    color: parent.highlighted ? theme.btnSecondaryHover : "transparent"
                }

                contentItem: Text {
                    text: parent.text
                    color: theme.textPrimary
                    font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                onClicked: {
                    ctrl.currentIndex = index
                    ctrl.popup.close()
                }
            }
        }
    }
}
