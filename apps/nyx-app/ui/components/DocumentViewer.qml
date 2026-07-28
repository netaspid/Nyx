import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "."

Item {
    id: root
    required property var theme
    required property var viewer
    z: 960
    anchors.fill: parent
    visible: viewer && viewer.open
    focus: visible
    readonly property bool narrow: width < 720 || Qt.platform.os === "android"

    Keys.onPressed: function(event) {
        if (!viewer || !viewer.open) return
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            viewer.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Left || event.key === Qt.Key_PageUp) {
            viewer.prevPage()
            event.accepted = true
        } else if (event.key === Qt.Key_Right || event.key === Qt.Key_PageDown) {
            viewer.nextPage()
            event.accepted = true
        } else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            viewer.zoomIn()
            event.accepted = true
        } else if (event.key === Qt.Key_Minus) {
            viewer.zoomOut()
            event.accepted = true
        }
    }

    onVisibleChanged: {
        if (visible)
            forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#e0080a0c"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.narrow ? 8 : 16
        spacing: root.narrow ? 8 : 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: viewer ? (viewer.title || qsTr("Документ")) : ""
                color: "#ffffff"
                font.pixelSize: root.narrow ? 14 : 16
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
            }
            NyxButtonSecondary {
                theme: root.theme
                text: qsTr("Внешне")
                visible: !root.narrow
                onClicked: if (viewer) viewer.openExternally()
            }
            IconButton {
                theme: root.theme
                name: "close"
                ToolTip.text: qsTr("Закрыть")
                onClicked: if (viewer) viewer.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: root.theme ? root.theme.bgPanel : "#1a1d21"
            border.color: root.theme ? root.theme.border : "#333"
            clip: true

            Column {
                anchors.centerIn: parent
                spacing: 10
                visible: viewer && (viewer.mode === "busy" || viewer.mode === "error")
                width: Math.min(parent.width * 0.8, 420)
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: viewer && viewer.mode === "busy"
                    visible: running
                }
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: viewer && viewer.mode === "error"
                           ? "#ff8a80" : (root.theme ? root.theme.textSecondary : "#ccc")
                    text: {
                        if (!viewer) return ""
                        if (viewer.mode === "error") return viewer.error || qsTr("Ошибка")
                        return viewer.status || qsTr("Загрузка…")
                    }
                }
                NyxButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    theme: root.theme
                    visible: viewer && viewer.mode === "error"
                    text: qsTr("Открыть внешне")
                    onClicked: if (viewer) viewer.openExternally()
                }
            }

            ScrollView {
                anchors.fill: parent
                anchors.margins: 12
                visible: viewer && viewer.mode === "text"
                clip: true
                TextArea {
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    text: viewer ? (viewer.text || "") : ""
                    color: root.theme ? root.theme.textPrimary : "#eee"
                    selectedTextColor: "#fff"
                    selectionColor: root.theme ? root.theme.accent : "#3d7eff"
                    background: null
                    font.family: "monospace"
                    font.pixelSize: 13
                }
            }

            Flickable {
                id: pdfFlick
                anchors.fill: parent
                anchors.margins: 8
                visible: viewer && viewer.mode === "pdf"
                contentWidth: Math.max(width, pdfPage.width)
                contentHeight: Math.max(height, pdfPage.height)
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Image {
                    id: pdfPage
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: (viewer && viewer.pageUrl) ? viewer.pageUrl : ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                    width: implicitWidth > 0 ? implicitWidth : 1
                    height: implicitHeight > 0 ? implicitHeight : 1
                }

                Label {
                    anchors.centerIn: parent
                    visible: viewer && viewer.mode === "pdf" && viewer.status.length > 0
                             && !(viewer.pageUrl && viewer.pageUrl.length)
                    text: viewer ? viewer.status : ""
                    color: root.theme ? root.theme.textMuted : "#999"
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: viewer && viewer.pageCount > 0
            spacing: 8

            NyxButtonSecondary {
                theme: root.theme
                text: "◀"
                enabled: viewer && viewer.canPrev
                onClicked: if (viewer) viewer.prevPage()
            }
            Label {
                text: viewer
                      ? qsTr("%1 / %2").arg(viewer.page).arg(viewer.pageCount)
                      : ""
                color: "#ffffff"
                font.pixelSize: 13
            }
            NyxButtonSecondary {
                theme: root.theme
                text: "▶"
                enabled: viewer && viewer.canNext
                onClicked: if (viewer) viewer.nextPage()
            }
            Item { Layout.fillWidth: true }
            NyxButtonSecondary {
                theme: root.theme
                text: "−"
                onClicked: if (viewer) viewer.zoomOut()
            }
            Label {
                text: viewer ? Math.round(viewer.zoom * 100) + "%" : ""
                color: "#ffffff"
                font.pixelSize: 13
            }
            NyxButtonSecondary {
                theme: root.theme
                text: "+"
                onClicked: if (viewer) viewer.zoomIn()
            }
        }
    }
}
