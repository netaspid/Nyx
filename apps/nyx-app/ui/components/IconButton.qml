import QtQuick
import QtQuick.Controls

/** Круглая кнопка с иконкой (Segoe MDL2 Assets). */
AbstractButton {
    id: ctrl

    property string glyph: "\uE946"
    property color iconColor: theme ? theme.textPrimary : "#ffffff"
    property var theme
    property int btnSize: 36

    implicitWidth: btnSize
    implicitHeight: btnSize
    hoverEnabled: true

    background: Rectangle {
        radius: btnSize / 2
        color: {
            if (!ctrl.enabled) return theme ? theme.inputBg : "#333"
            if (ctrl.pressed) return theme ? theme.accentPress : "#4674a8"
            if (ctrl.hovered) return theme ? theme.btnSecondaryHover : "#444"
            return "transparent"
        }
        border.color: theme ? theme.border : "#555"
        border.width: ctrl.enabled ? 0 : 1
    }

    contentItem: Text {
        text: ctrl.glyph
        font.family: "Segoe MDL2 Assets"
        font.pixelSize: btnSize * 0.42
        color: ctrl.enabled ? ctrl.iconColor : (theme ? theme.textMuted : "#888")
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
