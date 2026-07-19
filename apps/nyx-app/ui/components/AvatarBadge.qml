import QtQuick

/** Круглый аватар: фото или инициал (Telegram-style). */
Item {
    id: av
    property string label: "?"
    property color baseColor: "#5288c1"
    property color textColor: "#ffffff"
    property string imageSource: ""
    property int size: 40
    implicitWidth: size
    implicitHeight: size

    Rectangle {
        id: circle
        anchors.fill: parent
        radius: av.size / 2
        color: av.baseColor
        clip: true

        Rectangle {
            anchors.fill: parent
            radius: circle.radius
            visible: !photo.visible
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: Qt.lighter(av.baseColor, 1.18) }
                GradientStop { position: 1.0; color: av.baseColor }
            }
        }

        Image {
            id: photo
            anchors.fill: parent
            source: {
                if (!av.imageSource.length) return ""
                let s = String(av.imageSource).replace(/\\/g, "/")
                if (s.length >= 2 && s[1] === ":") return "file:///" + s
                if (s.startsWith("/")) return "file://" + s
                return "file:///" + s
            }
            fillMode: Image.PreserveAspectCrop
            visible: status === Image.Ready
            asynchronous: true
        }

        Text {
            anchors.centerIn: parent
            visible: !photo.visible
            text: label.length ? label[0].toUpperCase() : "?"
            color: av.textColor
            font.bold: true
            font.pixelSize: av.size * 0.42
        }
    }
}
