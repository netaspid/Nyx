import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/** Автодополнение @упоминаний над полем ввода. */
Popup {
    id: root
    required property var theme
    property var candidates: []

    signal picked(string nickname, string userId)

    modal: false
    focus: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 4
    width: 240
    height: Math.min(200, list.contentHeight + 8)

    background: Rectangle {
        radius: 10
        color: Qt.rgba(theme.toastBg.r, theme.toastBg.g, theme.toastBg.b, 0.96)
        border.color: theme.border
    }

    contentItem: ListView {
        id: list
        clip: true
        model: root.candidates
        spacing: 2
        delegate: Rectangle {
            required property var modelData
            width: list.width
            height: 36
            radius: 6
            color: ma.containsMouse ? theme.btnSecondaryHover : "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: modelData.nickname || modelData.idShort || "?"
                    color: theme.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: 13
                }
                Label {
                    text: modelData.idShort || ""
                    color: theme.textMuted
                    font.pixelSize: 10
                    font.family: "Consolas"
                }
            }
            MouseArea {
                id: ma
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.picked(modelData.nickname || modelData.idShort,
                                       modelData.userId)
            }
        }
    }

    function placeAbove(item) {
        if (!item || !Overlay.overlay) return
        const p = item.mapToItem(Overlay.overlay, 0, 0)
        width = Math.min(280, item.width)
        x = Math.max(8, p.x)
        y = Math.max(8, p.y - height - 6)
    }
}
