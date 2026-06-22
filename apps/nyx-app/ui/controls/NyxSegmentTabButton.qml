import QtQuick
import QtQuick.Controls

/** Сегментированная вкладка — единый стиль с сайдбаром. */
TabButton {
    id: ctrl
    required property var theme

    background: Rectangle {
        radius: theme.radiusBtn - 2
        color: ctrl.checked ? theme.accent
             : ctrl.hovered ? theme.btnSecondaryHover
             : "transparent"
    }

    contentItem: Label {
        text: ctrl.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: ctrl.checked ? theme.textPrimary : theme.textSecondary
        font.pixelSize: fontSize
        font.weight: ctrl.checked ? Font.DemiBold : Font.Normal
    }

    property int fontSize: 12
}
