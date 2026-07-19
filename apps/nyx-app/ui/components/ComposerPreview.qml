import QtQuick
import QtQuick.Layouts
import "../js/MarkdownFormat.js" as Md

/** Компактное «как увидят» для композера: блоки + Telegram-спойлеры. */
Item {
    id: root
    required property var theme
    property string sourceText: ""
    property var revealedSpoilers: ({})

    readonly property var blocks: sourceText.trim().length
                                  ? Md.parseBlocks(sourceText) : []
    readonly property real maxInnerW: Math.max(80, width - 4)

    implicitHeight: col.implicitHeight
    clip: true

    onSourceTextChanged: revealedSpoilers = ({})

    Column {
        id: col
        width: parent.width
        spacing: 4

        Text {
            visible: root.blocks.length === 0
            width: parent.width
            text: "…"
            color: theme.textMuted
            font.pixelSize: 13
            font.family: "Segoe UI"
        }

        Repeater {
            model: root.blocks

            Column {
                id: blockCol
                required property var modelData
                width: parent.width
                spacing: 2

                Text {
                    visible: blockCol.modelData.type === "action"
                    width: parent.width
                    text: "<i>" + Md.escapeHtml(blockCol.modelData.text || "") + "</i>"
                    color: theme.bubbleTextOut
                    font.pixelSize: 13
                    textFormat: Text.RichText
                    wrapMode: Text.Wrap
                }

                CodeBlockView {
                    visible: blockCol.modelData.type === "code"
                    width: parent.width
                    lang: blockCol.modelData.caption || ""
                    code: blockCol.modelData.text || ""
                    maxContentHeight: 120
                }

                Text {
                    visible: blockCol.modelData.type === "table"
                             || blockCol.modelData.type === "formula"
                             || blockCol.modelData.type === "media"
                    width: parent.width
                    text: {
                        const b = blockCol.modelData
                        if (b.type === "table") return Md.tableToHtml(b.text)
                        if (b.type === "formula")
                            return "<div style=\"text-align:center;\">"
                                   + Md.formulaToHtml(b.text) + "</div>"
                        return "[" + Md.escapeHtml(b.caption || "media") + "]"
                    }
                    color: theme.bubbleTextOut
                    font.pixelSize: 13
                    textFormat: Text.RichText
                    wrapMode: Text.Wrap
                }

                Flow {
                    visible: blockCol.modelData.type === "paragraph"
                    width: parent.width
                    spacing: 3

                    Repeater {
                        model: blockCol.modelData.type === "paragraph"
                               ? Md.splitSpoilers(blockCol.modelData.text) : []

                        Item {
                            id: segItem
                            required property var modelData
                            width: segInner.implicitWidth
                            height: segInner.implicitHeight

                            Item {
                                id: segInner
                                implicitWidth: spoiler.visible ? spoiler.implicitWidth
                                               : mdText.implicitWidth
                                implicitHeight: spoiler.visible ? spoiler.implicitHeight
                                                : mdText.implicitHeight

                                SpoilerSpan {
                                    id: spoiler
                                    visible: segItem.modelData.type === "spoiler"
                                    body: segItem.modelData.text || ""
                                    revealed: !!(root.revealedSpoilers[segItem.modelData.index])
                                    bubbleColor: theme.bubbleOut
                                    textColor: theme.bubbleTextOut
                                    maxWidth: root.maxInnerW
                                    onRevealRequested: {
                                        const next = Object.assign({}, root.revealedSpoilers)
                                        next[segItem.modelData.index] = true
                                        root.revealedSpoilers = next
                                    }
                                }

                                Text {
                                    id: mdText
                                    visible: segItem.modelData.type === "md"
                                    width: Math.min(root.maxInnerW,
                                                    Math.max(1, implicitWidth))
                                    text: Md.toHtml(segItem.modelData.text || "", {}, true)
                                    color: theme.bubbleTextOut
                                    font.pixelSize: 13
                                    font.family: "Segoe UI"
                                    wrapMode: Text.Wrap
                                    textFormat: Text.RichText
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
