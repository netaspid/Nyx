import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "."

Popup {
    id: root
    required property var theme
    required property var node
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    width: Overlay.overlay ? Overlay.overlay.width : 400
    height: Overlay.overlay ? Overlay.overlay.height : 600
    padding: 0

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            root.closeSafely()
            event.accepted = true
        }
    }

    property bool circleMode: true
    property string state: "idle"
    property string pendingPath: ""
    property bool acceptOnStop: false
    property bool closeOnStop: false
    property real startedAt: 0
    property int elapsedMs: 0
    readonly property bool busy: state === "starting" || state === "recording"
                                 || state === "stopping"

    function resetCapture(removeFile) {
        stateTimer.stop()
        durationTimer.stop()
        previewPlayer.stop()
        previewPlayer.source = ""
        if (removeFile && pendingPath.length)
            node.removeStagingMedia(pendingPath)
        pendingPath = ""
        acceptOnStop = false
        closeOnStop = false
        startedAt = 0
        elapsedMs = 0
        state = "idle"
    }

    function requestStart() {
        if (busy || state === "preview") return
        errorLabel.text = ""
        state = "starting"
        stateTimer.interval = 8000
        stateTimer.start()
        node.requestChatCapturePermissions(true)
    }

    function startAfterPermission() {
        if (state !== "starting") return
        if (!circleMode) {
            const photoPath = node.chatCaptureStagingPath("jpg")
            imageCapture.captureToFile(photoPath)
            return
        }
        pendingPath = node.chatCaptureStagingPath("mp4")
        node.removeStagingMedia(pendingPath)
        recorder.outputLocation = "file://" + pendingPath
        startedAt = Date.now()
        elapsedMs = 0
        recorder.record()
    }

    function stopRecording(sendAfterStop) {
        if (state !== "recording") return
        acceptOnStop = !!sendAfterStop
        state = "stopping"
        durationTimer.stop()
        stateTimer.interval = 10000
        stateTimer.restart()
        recorder.stop()
    }

    function fail(message) {
        errorLabel.text = message || qsTr("Не удалось завершить запись")
        state = "failed"
        stateTimer.stop()
        durationTimer.stop()
        acceptOnStop = false
    }

    function closeSafely() {
        if (state === "recording") {
            closeOnStop = true
            stopRecording(false)
            return
        }
        if (state === "stopping") {
            closeOnStop = true
            acceptOnStop = false
            return
        }
        resetCapture(true)
        close()
    }

    function selectFrontCamera() {
        const cameras = mediaDevices.videoInputs
        if (!cameras || cameras.length === 0) return
        for (let i = 0; i < cameras.length; ++i) {
            if (cameras[i].position === CameraDevice.FrontFace) {
                if (camera.cameraDevice.id === cameras[i].id)
                    return
                const reactivate = root.opened && root.state !== "preview"
                camera.active = false
                camera.cameraDevice = cameras[i]
                if (reactivate) {
                    Qt.callLater(function() {
                        camera.active = true
                    })
                }
                return
            }
        }
    }

    Component.onCompleted: selectFrontCamera()

    onOpened: {
        errorLabel.text = ""
        circleMode = true
        resetCapture(true)
    }
    onClosed: {
        previewPlayer.stop()
        if (state !== "stopping")
            resetCapture(true)
    }

    Connections {
        target: root.node
        function onChatCapturePermissionResult(granted) {
            if (root.state !== "starting") return
            if (!granted) {
                root.fail(qsTr("Нет разрешения на камеру или микрофон"))
                return
            }
            root.startAfterPermission()
        }
    }

    Timer {
        id: durationTimer
        interval: 100
        repeat: true
        onTriggered: {
            root.elapsedMs = Date.now() - root.startedAt
            if (root.elapsedMs >= 60000)
                root.stopRecording(true)
        }
    }

    Timer {
        id: stateTimer
        repeat: false
        onTriggered: root.fail(qsTr("Камера не ответила вовремя"))
    }

    background: Rectangle { color: "#f0080a0c" }

    contentItem: Item {
        anchors.fill: parent

        CaptureSession {
            id: captureSession
            camera: camera
            audioInput: root.circleMode ? captureAudioInput : null
            imageCapture: root.circleMode ? null : imageCapture
            recorder: root.circleMode ? recorder : null
            videoOutput: livePreview
        }

        AudioInput {
            id: captureAudioInput
        }

        MediaDevices { id: mediaDevices }

        Camera {
            id: camera
            active: root.opened && root.state !== "preview"
            onErrorOccurred: function(error, message) {
                root.fail(message || qsTr("Камера недоступна"))
            }
        }

        ImageCapture {
            id: imageCapture
            onImageSaved: function(id, path) {
                stateTimer.stop()
                root.close()
                root.node.sendCapturedMedia(path, "image/jpeg", "photo.jpg", "photo")
            }
            onErrorOccurred: function(id, error, message) {
                root.fail(message || qsTr("Не удалось сделать фото"))
            }
        }

        MediaRecorder {
            id: recorder
            quality: MediaRecorder.NormalQuality
            onRecorderStateChanged: function() {
                if (recorder.recorderState === MediaRecorder.RecordingState
                        && root.state === "starting") {
                    stateTimer.stop()
                    root.state = "recording"
                    root.startedAt = Date.now()
                    durationTimer.start()
                    return
                }
                if (recorder.recorderState !== MediaRecorder.StoppedState
                        || root.state !== "stopping")
                    return

                stateTimer.stop()
                if (root.closeOnStop) {
                    root.resetCapture(true)
                    root.close()
                    return
                }
                if (!root.acceptOnStop || root.elapsedMs < 700) {
                    root.resetCapture(true)
                    return
                }
                root.state = "preview"
                previewPlayer.source = "file://" + root.pendingPath
            }
            onErrorOccurred: function(error, message) {
                root.fail(message || qsTr("Не удалось записать видеокружок"))
            }
        }

        MediaPlayer {
            id: previewPlayer
            videoOutput: recordedPreview
            audioOutput: AudioOutput {}
        }

        Item {
            id: circleFrame
            anchors.centerIn: parent
            width: Math.min(parent.width - 32, parent.height - 190, 440)
            height: width
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "#11151c"
                clip: true

                VideoOutput {
                    id: livePreview
                    anchors.fill: parent
                    visible: root.state !== "preview"
                    fillMode: VideoOutput.PreserveAspectCrop
                    orientation: 0
                }

                VideoOutput {
                    id: recordedPreview
                    anchors.fill: parent
                    visible: root.state === "preview"
                    fillMode: VideoOutput.PreserveAspectCrop
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "transparent"
                border.color: root.state === "recording" ? "#ef5350" : "#66ffffff"
                border.width: root.state === "recording" ? 5 : 2
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: root.circleMode ? qsTr("Видеокружок") : qsTr("Фото")
                    color: "#ffffff"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                IconButton {
                    theme: root.theme
                    name: "close"
                    onClicked: root.closeSafely()
                }
            }

            Item { Layout.fillHeight: true }

            Label {
                id: errorLabel
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                color: "#ff8a80"
                wrapMode: Text.WordWrap
                visible: text.length > 0
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                visible: root.state === "recording"
                text: {
                    const sec = Math.floor(root.elapsedMs / 1000)
                    return qsTr("%1:%2 / 1:00")
                            .arg(Math.floor(sec / 60))
                            .arg(String(sec % 60).padStart(2, "0"))
                }
                color: "#ffffff"
                font.pixelSize: 14
                font.bold: true
            }

            MediaPlaybackControls {
                Layout.fillWidth: true
                Layout.maximumWidth: 440
                Layout.alignment: Qt.AlignHCenter
                visible: root.state === "preview"
                theme: root.theme
                player: previewPlayer
                compact: width < 320
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 20

                IconButton {
                    theme: root.theme
                    name: root.circleMode ? "image" : "video"
                    btnSize: 48
                    enabled: !root.busy && root.state !== "preview"
                    onClicked: root.circleMode = !root.circleMode
                }

                IconButton {
                    theme: root.theme
                    name: root.state === "recording" ? "stop"
                          : (root.state === "preview" ? "send" : "video")
                    btnSize: 72
                    accent: root.state === "recording"
                    enabled: root.state !== "starting" && root.state !== "stopping"
                    onClicked: {
                        if (root.state === "recording") {
                            root.stopRecording(true)
                        } else if (root.state === "preview") {
                            const path = root.pendingPath
                            root.pendingPath = ""
                            root.close()
                            root.node.sendCapturedMedia(
                                        path, "video/mp4",
                                        "circle-message.mp4", "circle")
                        } else {
                            root.requestStart()
                        }
                    }
                }

                IconButton {
                    theme: root.theme
                    name: root.state === "preview" ? "close" : "camera-switch"
                    btnSize: 48
                    enabled: !root.busy
                    onClicked: {
                        if (root.state === "preview") {
                            previewPlayer.stop()
                            root.resetCapture(true)
                            return
                        }
                        const cams = mediaDevices.videoInputs
                        if (!cams || cams.length < 2) return
                        const current = camera.cameraDevice
                        let next = cams[0]
                        for (let i = 0; i < cams.length; ++i) {
                            if (cams[i].id === current.id) {
                                next = cams[(i + 1) % cams.length]
                                break
                            }
                        }
                        camera.active = false
                        camera.cameraDevice = next
                        Qt.callLater(function() {
                            if (root.opened && root.state !== "preview")
                                camera.active = true
                        })
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    if (root.state === "starting") return qsTr("Запуск камеры…")
                    if (root.state === "stopping") return qsTr("Сохранение записи…")
                    if (root.state === "recording") return qsTr("Нажмите ■, чтобы завершить")
                    if (root.state === "preview") return qsTr("Проверьте кружок перед отправкой")
                    return root.circleMode ? qsTr("Нажмите для записи видеокружка")
                                           : qsTr("Нажмите, чтобы сделать фото")
                }
                color: "#ccffffff"
                font.pixelSize: 12
            }
        }
    }
}
