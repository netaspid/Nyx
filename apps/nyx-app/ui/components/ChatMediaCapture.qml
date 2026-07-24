import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "../controls"
import "."

/** Camera capture overlay: photo or short video → sendCapturedMedia. */
Popup {
    id: root
    required property var theme
    required property var node
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Overlay.overlay ? Overlay.overlay.width : 400
    height: Overlay.overlay ? Overlay.overlay.height : 600
    padding: 0

    property bool videoMode: false
    property bool recording: false
    property string pendingPath: ""
    property string pendingMime: ""

    onOpened: {
        errorLabel.text = ""
        videoMode = false
        recording = false
    }
    onClosed: {
        if (recording) {
            recorder.stop()
            recording = false
        }
        pendingPath = ""
        pendingMime = ""
    }

    function finishCapture(path, mime, name) {
        root.close()
        if (!path || !path.length) return
        node.sendCapturedMedia(path, mime || "", name || "")
    }

    background: Rectangle { color: "#e0080a0c" }

    contentItem: Item {
        anchors.fill: parent

        CaptureSession {
            id: captureSession
            camera: camera
            imageCapture: imageCapture
            recorder: recorder
            videoOutput: preview
        }

        MediaDevices {
            id: mediaDevices
        }

        Camera {
            id: camera
            active: root.opened
        }

        ImageCapture {
            id: imageCapture
            onImageSaved: function(id, path) {
                root.finishCapture(path, "image/jpeg", "photo.jpg")
            }
            onErrorOccurred: function(id, error, message) {
                errorLabel.text = message || qsTr("Не удалось сделать фото")
            }
        }

        MediaRecorder {
            id: recorder
            onRecorderStateChanged: {
                if (recorder.recorderState === MediaRecorder.StoppedState && root.recording) {
                    root.recording = false
                    const path = recorder.actualLocation
                                   ? recorder.actualLocation.toLocalFile()
                                   : root.pendingPath
                    if (path && path.length)
                        root.finishCapture(path, "video/mp4", "video.mp4")
                }
            }
            onErrorOccurred: function(error, message) {
                root.recording = false
                errorLabel.text = message || qsTr("Не удалось записать видео")
            }
        }

        VideoOutput {
            id: preview
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectCrop
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: root.videoMode ? qsTr("Видео") : qsTr("Фото")
                    color: "#ffffff"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                IconButton {
                    theme: root.theme
                    name: "close"
                    onClicked: root.close()
                }
            }

            Item { Layout.fillHeight: true }

            Label {
                id: errorLabel
                Layout.fillWidth: true
                color: "#ff8a80"
                wrapMode: Text.WordWrap
                visible: text.length > 0
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 18

                // Mode toggle
                Rectangle {
                    width: 48
                    height: 48
                    radius: 24
                    color: "#cc2a3140"
                    border.color: "#55ffffff"
                    NyxIcon {
                        anchors.centerIn: parent
                        name: root.videoMode ? "image" : "video"
                        width: 22
                        height: 22
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.recording
                        onClicked: root.videoMode = !root.videoMode
                    }
                }

                // Shutter
                Rectangle {
                    width: 72
                    height: 72
                    radius: 36
                    color: root.recording ? "#e53935" : "#ffffff"
                    border.color: root.videoMode ? "#e53935" : "#ffffff"
                    border.width: 4
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            errorLabel.text = ""
                            if (!root.videoMode) {
                                const dest = node.chatCaptureStagingPath("jpg")
                                imageCapture.captureToFile(dest)
                                return
                            }
                            if (root.recording) {
                                recorder.stop()
                                return
                            }
                            const dest = node.chatCaptureStagingPath("mp4")
                            root.pendingPath = dest
                            recorder.outputLocation = "file://" + dest
                            root.recording = true
                            recorder.record()
                        }
                    }
                }

                Rectangle {
                    width: 48
                    height: 48
                    radius: 24
                    color: "#cc2a3140"
                    border.color: "#55ffffff"
                    visible: typeof camera.cameraDevice !== "undefined"
                    NyxIcon {
                        anchors.centerIn: parent
                        name: "camera-switch"
                        width: 22
                        height: 22
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.recording
                        onClicked: {
                            const cams = mediaDevices.videoInputs
                            if (!cams || cams.length < 2) return
                            const cur = camera.cameraDevice
                            let next = cams[0]
                            for (let i = 0; i < cams.length; ++i) {
                                if (cams[i].id === cur.id) {
                                    next = cams[(i + 1) % cams.length]
                                    break
                                }
                            }
                            camera.cameraDevice = next
                        }
                    }
                }
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: root.recording
                      ? qsTr("Идёт запись — нажмите ещё раз, чтобы отправить")
                      : (root.videoMode
                         ? qsTr("Нажмите, чтобы начать видео")
                         : qsTr("Нажмите, чтобы сделать фото"))
                color: "#ccffffff"
                font.pixelSize: 12
            }
        }
    }
}
