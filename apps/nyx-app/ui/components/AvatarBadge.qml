import QtQuick

/** Круглый аватар с инициалом (Telegram-style). */
Rectangle {
    id: av
    property string label: "?"
    property color baseColor: "#5288c1"
    property color textColor: "#ffffff"
    implicitWidth: size
    implicitHeight: size
    property int size: 40
    radius: size / 2
    color: baseColor

    Rectangle {
        anchors.fill: parent
        radius: av.radius
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Qt.lighter(av.baseColor, 1.18) }
            GradientStop { position: 1.0; color: av.baseColor }
        }
    }

    Text {
        anchors.centerIn: parent
        text: label.length ? label[0].toUpperCase() : "?"
        color: av.textColor
        font.bold: true
        font.pixelSize: av.size * 0.42
    }
}
