import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/**
 * Теги-облачка: пробел / Enter / запятая создают чип; Backspace на пустом — снять последний.
 * tagsText — строка через запятую (совместимо с хранилищем меты).
 */
Item {
    id: root
    required property var theme
    property bool readOnly: false
    property string tagsText: ""
    property string placeholderText: qsTr("Тег и пробел")

    readonly property var palette: [
        "#5288c1", "#6ab2f2", "#7b68ee", "#5b9a8b", "#c27856", "#9b59b6",
        "#e67e22", "#16a085", "#c0392b", "#2980b9"
    ]

    implicitHeight: Math.max(readOnly ? 24 : 40, box.implicitHeight)
    implicitWidth: 200

    function tagColor(name) {
        let hash = 0
        const s = String(name || "")
        for (let i = 0; i < s.length; ++i)
            hash = ((hash << 5) - hash + s.charCodeAt(i)) | 0
        return palette[Math.abs(hash) % palette.length]
    }

    function parseTags(text) {
        const raw = String(text || "").split(/[\s,;]+/)
        const out = []
        const seen = {}
        for (let i = 0; i < raw.length; ++i) {
            const t = raw[i].trim()
            if (!t) continue
            const key = t.toLowerCase()
            if (seen[key]) continue
            seen[key] = true
            out.push(t)
        }
        return out
    }

    function joinTags(list) {
        return list.join(", ")
    }

    function syncFromText() {
        const next = parseTags(tagsText)
        const cur = []
        for (let i = 0; i < tagsModel.count; ++i)
            cur.push(tagsModel.get(i).name)
        if (cur.join("\u0001") === next.join("\u0001")) return
        tagsModel.clear()
        for (let j = 0; j < next.length; ++j)
            tagsModel.append({ name: next[j] })
    }

    function emitTags() {
        const list = []
        for (let i = 0; i < tagsModel.count; ++i)
            list.push(tagsModel.get(i).name)
        const joined = joinTags(list)
        if (joined !== tagsText)
            tagsText = joined
    }

    function addTag(raw) {
        if (readOnly) return
        const t = String(raw || "").trim()
        if (!t) return
        const key = t.toLowerCase()
        for (let i = 0; i < tagsModel.count; ++i) {
            if (String(tagsModel.get(i).name).toLowerCase() === key)
                return
        }
        tagsModel.append({ name: t })
        emitTags()
    }

    function removeAt(index) {
        if (readOnly) return
        if (index < 0 || index >= tagsModel.count) return
        tagsModel.remove(index)
        emitTags()
    }

    function commitInput() {
        const t = String(input.text || "")
        input.text = ""
        const parts = parseTags(t)
        for (let i = 0; i < parts.length; ++i)
            addTag(parts[i])
    }

    onTagsTextChanged: syncFromText()
    Component.onCompleted: syncFromText()

    ListModel { id: tagsModel }

    Rectangle {
        id: box
        anchors.fill: parent
        implicitHeight: flow.implicitHeight + (root.readOnly ? 0 : 14)
        radius: theme.radiusInput
        color: root.readOnly ? "transparent" : theme.inputBg
        border.color: root.readOnly ? "transparent"
                     : (input.activeFocus ? theme.focusRing : theme.border)
        border.width: root.readOnly ? 0 : (input.activeFocus ? 2 : 1)

        Flow {
            id: flow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: root.readOnly ? 0 : 7
            spacing: 6

            Repeater {
                model: tagsModel
                delegate: TagChip {
                    required property int index
                    required property string name
                    theme: root.theme
                    text: name
                    chipColor: root.tagColor(name)
                    removable: !root.readOnly
                    onRemoveRequested: root.removeAt(index)
                }
            }

            TextInput {
                id: input
                visible: !root.readOnly
                width: Math.max(120, Math.min(200, contentWidth + 28))
                height: 26
                verticalAlignment: TextInput.AlignVCenter
                color: theme.textPrimary
                selectionColor: theme.accent
                selectedTextColor: theme.textPrimary
                font.pixelSize: 13
                clip: true
                focus: false

                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    text: root.placeholderText
                    color: theme.textMuted
                    font.pixelSize: 12
                    visible: !input.text.length && tagsModel.count === 0
                }

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Backspace && input.text.length === 0
                            && tagsModel.count > 0) {
                        root.removeAt(tagsModel.count - 1)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter || event.key === Qt.Key_Tab) {
                        root.commitInput()
                        event.accepted = true
                        return
                    }
                }

                onTextChanged: {
                    // IME / вставка: «a b», «a,b»
                    if (/[\s,;]/.test(text))
                        Qt.callLater(function() { root.commitInput() })
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: !root.readOnly
            z: -1
            onClicked: input.forceActiveFocus()
        }
    }
}
