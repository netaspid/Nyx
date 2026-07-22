import QtQuick
import QtQuick.Controls
import "../components"

ComboBox {
    id: ctrl
    required property var theme

    implicitHeight: 36

    palette.button: theme.inputBg
    palette.buttonText: theme.textPrimary
    palette.highlight: theme.btnSecondaryHover
    palette.highlightedText: theme.textPrimary
    palette.base: theme.bgSidebar
    palette.text: theme.textPrimary
    palette.window: theme.bgSidebar
    palette.windowText: theme.textPrimary

    /** Строка модели по индексу (ListModel или QVariantList/массив). */
    function rowAt(index) {
        if (!model || index < 0)
            return null
        if (typeof model.get === "function")
            return model.get(index)
        if (model.length !== undefined)
            return model[index]
        return null
    }

    /** Подпись пункта по индексу и textRole. */
    function itemTextAt(index) {
        const row = rowAt(index)
        if (!row)
            return ""
        if (textRole && textRole.length > 0 && typeof row === "object")
            return row[textRole] || ""
        return String(row)
    }

    /** Текст выбранного пункта; displayText не работает с QVariantList из C++. */
    readonly property string labelText: {
        if (displayText.length > 0)
            return displayText
        return itemTextAt(currentIndex)
    }

    background: Rectangle {
        radius: theme.radiusInput
        color: theme.inputBg
        border.color: ctrl.activeFocus ? theme.focusRing : theme.border
        border.width: ctrl.activeFocus ? 2 : 1
    }

    contentItem: Text {
        leftPadding: 10
        rightPadding: ctrl.indicator.width + 8
        text: ctrl.labelText
        color: theme.textPrimary
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: NyxIcon {
        x: ctrl.width - width - 10
        y: (ctrl.height - height) / 2
        name: "chevron"
        width: 12
        height: 12
        opacity: 0.7
    }

    popup: Popup {
        y: ctrl.height + 2
        width: ctrl.width
        padding: 4

        palette.window: theme.bgSidebar
        palette.text: theme.textPrimary
        palette.highlight: theme.btnSecondaryHover
        palette.highlightedText: theme.textPrimary

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
                id: del
                width: ListView.view.width - 8
                height: 36
                leftPadding: 8
                rightPadding: 8
                highlighted: ctrl.highlightedIndex === index

                readonly property string itemLabel: ctrl.itemTextAt(index)

                background: Rectangle {
                    radius: theme.radiusBtn - 2
                    color: del.highlighted ? theme.btnSecondaryHover
                         : (del.hovered ? theme.btnSecondary : "transparent")
                }

                contentItem: Text {
                    text: del.itemLabel
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
