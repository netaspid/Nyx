import QtQuick

/** SVG-иконка из resources/icons/ (24px). */
Image {
    id: root
    property string name: "send"
    width: 24
    height: 24
    source: "qrc:/icons/" + name + ".svg"
    sourceSize: Qt.size(24, 24)
    fillMode: Image.PreserveAspectFit
}
