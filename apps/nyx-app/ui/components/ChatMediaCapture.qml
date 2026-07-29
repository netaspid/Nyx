import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Effects
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
    property bool cameraWanted: false
    property bool cameraReady: false
    property string permissionPurpose: ""
    property var pendingCameraDevice: null
    property var lockedContentOrientation: undefined
    readonly property bool nativeCapture: Qt.platform.os === "android"
    readonly property bool busy: state === "starting" || state === "recording"
                                 || state === "stopping" || state === "teardown"
    readonly property bool showLiveCamera: state !== "preview" && state !== "teardown"

    function hostWindow() {
        return root.Window.window
    }

    function lockRecordingOrientation() {
        const win = hostWindow()
        if (!win) return
        if (lockedContentOrientation === undefined)
            lockedContentOrientation = win.contentOrientation
        win.contentOrientation = Screen.orientation
    }

    function unlockRecordingOrientation() {
        const win = hostWindow()
        if (!win || lockedContentOrientation === undefined) return
        win.contentOrientation = lockedContentOrientation
        lockedContentOrientation = undefined
    }

    function resetCapture(removeFile) {
        stateTimer.stop()
        durationTimer.stop()
        teardownTimer.stop()
        previewPlayer.stop()
        previewPlayer.source = ""
        if (removeFile && pendingPath.length)
            node.removeStagingMedia(pendingPath)
        pendingPath = ""
        acceptOnStop = false
        closeOnStop = false
        startedAt = 0
        elapsedMs = 0
        unlockRecordingOrientation()
        state = "idle"
    }

    function requestStart() {
        if (busy || state === "preview") return
        errorLabel.text = ""
        if (nativeCapture) {
            pendingPath = node.chatCaptureStagingPath("mp4")
            node.chatVideoRecorder.startRecording(pendingPath)
            return
        }
        if (!cameraReady) {
            permissionPurpose = "record"
            state = "starting"
            stateTimer.interval = 10000
            stateTimer.start()
            node.requestChatCapturePermissions(true)
            return
        }
        state = "starting"
        stateTimer.interval = 10000
        stateTimer.restart()
        startAfterPermission()
    }

    function restartCamera(device) {
        cameraReady = false
        cameraWanted = false
        pendingCameraDevice = device || null
        cameraRestartTimer.restart()
    }

    function startAfterPermission() {
        if (!cameraReady) {
            permissionPurpose = "record"
            cameraWanted = true
            state = "starting"
            stateTimer.interval = 10000
            stateTimer.restart()
            return
        }
        if (!circleMode) {
            const photoPath = node.chatCaptureStagingPath("jpg")
            imageCapture.captureToFile(photoPath)
            return
        }
        pendingPath = node.chatCaptureStagingPath("mp4")
        node.removeStagingMedia(pendingPath)
        recorder.outputLocation = "file://" + pendingPath
        lockRecordingOrientation()
        startedAt = Date.now()
        elapsedMs = 0
        recorder.record()
    }

    function stopRecording(sendAfterStop) {
        if (state !== "recording") return
        acceptOnStop = !!sendAfterStop
        if (nativeCapture) {
            node.chatVideoRecorder.stopRecording()
            return
        }
        state = "stopping"
        durationTimer.stop()
        stateTimer.interval = 12000
        stateTimer.restart()
        Qt.callLater(function() {
            if (root.state === "stopping")
                recorder.stop()
        })
    }

    function fail(message) {
        errorLabel.text = message || qsTr("Не удалось завершить запись")
        state = "failed"
        stateTimer.stop()
        durationTimer.stop()
        acceptOnStop = false
        unlockRecordingOrientation()
        cameraReady = false
        cameraWanted = false
    }

    function beginTeardownThenClose(removeFile) {
        if (state === "teardown") return
        state = "teardown"
        stateTimer.stop()
        durationTimer.stop()
        previewPlayer.stop()
        previewPlayer.source = ""
        unlockRecordingOrientation()
        if (recorder.recorderState !== MediaRecorder.StoppedState) {
            try { recorder.stop() } catch (e) {}
        }
        cameraReady = false
        cameraWanted = false
        teardownTimer.removeFile = !!removeFile
        teardownTimer.start()
    }

    function closeSafely() {
        if (nativeCapture) {
            node.chatVideoRecorder.close()
            pendingPath = ""
            close()
            return
        }
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
        beginTeardownThenClose(true)
    }

    function selectFrontCamera() {
        const cameras = mediaDevices.videoInputs
        if (!cameras || cameras.length === 0) return
        for (let i = 0; i < cameras.length; ++i) {
            if (cameras[i].position === CameraDevice.FrontFace) {
                if (camera.cameraDevice.id === cameras[i].id)
                    return
                camera.cameraDevice = cameras[i]
                return
            }
        }
    }

    function resolveRecordedPath() {
        const actual = recorder.actualLocation
                ? recorder.actualLocation.toString() : ""
        if (actual.indexOf("file:") === 0) {
            let p = decodeURIComponent(actual.replace(/^file:\/\
            if (Qt.platform.os === "windows" && p.charAt(0) === "/")
                p = p.substring(1)
            if (p.length)
                return p
        }
        return pendingPath
    }

    function enterPreview() {
        unlockRecordingOrientation()
        const path = resolveRecordedPath()
        if (path.length)
            pendingPath = path
        state = "preview"
        previewPlayer.source = ""
        Qt.callLater(function() {
            if (root.state !== "preview") return
            previewPlayer.source = "file://" + root.pendingPath
        })
    }

    Component.onCompleted: selectFrontCamera()

    onOpened: {
        errorLabel.text = ""
        circleMode = true
        resetCapture(true)
        if (nativeCapture) {
            state = "starting"
            node.chatVideoRecorder.openPreview()
            return
        }
        state = "starting"
        permissionPurpose = "preview"
        openDelayTimer.restart()
    }
    onClosed: {
        previewPlayer.stop()
        unlockRecordingOrientation()
        if (nativeCapture) {
            node.chatVideoRecorder.close()
            return
        }
        cameraReady = false
        cameraWanted = false
        if (state !== "stopping" && state !== "teardown")
            resetCapture(true)
    }

    Connections {
        target: root.node
        function onChatCapturePermissionResult(granted) {
            if (root.nativeCapture) return
            if (root.state !== "starting") return
            if (!granted) {
                root.fail(qsTr("Нет разрешения на камеру или микрофон"))
                return
            }
            if (!mediaDevices.videoInputs
                    || mediaDevices.videoInputs.length === 0) {
                root.fail(qsTr("Камера не найдена"))
                return
            }
            if (root.circleMode && (!mediaDevices.audioInputs
                    || mediaDevices.audioInputs.length === 0)) {
                root.fail(qsTr("Микрофон не найден"))
                return
            }
            root.cameraWanted = true
            root.cameraReady = false
            root.stateTimer.interval = 10000
            root.stateTimer.restart()
            if (root.permissionPurpose === "record")
                return
            root.permissionPurpose = ""
        }
    }

    Connections {
        target: root.node.chatVideoRecorder
        function onStateChanged() {
            if (!root.nativeCapture) return
            const recorder = root.node.chatVideoRecorder
            const next = recorder.state
            errorLabel.text = recorder.error || ""
            if (next === "starting-recording")
                root.state = "starting"
            else
                root.state = next
            if (next === "preview") {
                root.pendingPath = recorder.outputPath
                previewPlayer.source = "file://" + recorder.outputPath
            }
        }
        function onElapsedChanged() {
            if (root.nativeCapture)
                root.elapsedMs = root.node.chatVideoRecorder.elapsedMs
        }
        function onReady(path, mime, displayName, mediaKind) {
            if (!root.nativeCapture) return
            root.pendingPath = ""
            root.node.sendCapturedMedia(path, mime, displayName, mediaKind)
            root.close()
        }
    }

    Timer {
        id: openDelayTimer
        interval: 350
        repeat: false
        onTriggered: {
            if (!root.opened || root.state !== "starting") return
            root.node.requestChatCapturePermissions(true)
        }
    }

    Timer {
        id: cameraWarmupTimer
        interval: 700
        repeat: false
        onTriggered: {
            if (!root.opened || !root.cameraWanted || !camera.active) return
            root.cameraReady = true
            root.stateTimer.stop()
            if (root.permissionPurpose === "record") {
                root.permissionPurpose = ""
                root.startAfterPermission()
            } else {
                root.state = "idle"
            }
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

    Timer {
        id: teardownTimer
        property bool removeFile: true
        interval: 250
        repeat: false
        onTriggered: {
            Qt.callLater(function() {
                stateTimer.stop()
                durationTimer.stop()
                previewPlayer.stop()
                previewPlayer.source = ""
                if (teardownTimer.removeFile && root.pendingPath.length)
                    root.node.removeStagingMedia(root.pendingPath)
                root.pendingPath = ""
                root.acceptOnStop = false
                root.closeOnStop = false
                root.startedAt = 0
                root.elapsedMs = 0
                root.unlockRecordingOrientation()
                root.state = "idle"
                root.close()
            })
        }
    }

    background: Rectangle { color: "#f0080a0c" }

    contentItem: Item {
        anchors.fill: parent

        CaptureSession {
            id: captureSession
            camera: root.nativeCapture ? null : camera
            audioInput: !root.nativeCapture && root.circleMode ? captureAudioInput : null
            imageCapture: !root.nativeCapture && !root.circleMode ? imageCapture : null
            recorder: !root.nativeCapture && root.circleMode ? recorder : null
            videoOutput: root.nativeCapture ? null : livePreview
        }

        AudioInput {
            id: captureAudioInput
        }

        MediaDevices { id: mediaDevices }

        Camera {
            id: camera
            active: !root.nativeCapture && root.cameraWanted
            onActiveChanged: {
                if (active)
                    cameraWarmupTimer.restart()
                else
                    root.cameraReady = false
            }
            onErrorOccurred: function(error, message) {
                root.cameraReady = false
                root.cameraWanted = false
                root.fail(message || qsTr("Камера недоступна"))
            }
        }

        ImageCapture {
            id: imageCapture
            onImageSaved: function(id, path) {
                stateTimer.stop()
                root.beginTeardownThenClose(false)
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
                    root.beginTeardownThenClose(true)
                    return
                }
                if (!root.acceptOnStop || root.elapsedMs < 700) {
                    root.resetCapture(true)
                    return
                }
                root.enterPreview()
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
                id: captureSurface
                anchors.fill: parent
                radius: width / 2
                color: "#11151c"
                clip: true
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: captureMask
                }

                VideoOutput {
                    id: livePreview
                    anchors.fill: parent
                    visible: !root.nativeCapture && root.state !== "preview"
                    fillMode: VideoOutput.PreserveAspectCrop
                }

                Image {
                    anchors.fill: parent
                    visible: root.nativeCapture && root.state !== "preview"
                    source: visible
                            ? "image://nyxcapture/local/"
                              + root.node.chatVideoRecorder.frameEpoch
                            : ""
                    fillMode: Image.PreserveAspectCrop
                    cache: false
                    asynchronous: false
                }

                VideoOutput {
                    id: recordedPreview
                    anchors.fill: parent
                    visible: root.state === "preview"
                    fillMode: VideoOutput.PreserveAspectCrop
                }
            }

            Rectangle {
                id: captureMask
                anchors.fill: parent
                radius: width / 2
                color: "white"
                visible: false
                layer.enabled: true
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
                    visible: !root.nativeCapture
                    enabled: !root.busy && root.state !== "preview"
                    onClicked: {
                        root.circleMode = !root.circleMode
                        root.restartCamera(null)
                    }
                }

                IconButton {
                    theme: root.theme
                    name: root.state === "recording" ? "stop"
                          : (root.state === "preview" ? "send" : "video")
                    btnSize: 72
                    accent: root.state === "recording"
                    enabled: root.state !== "starting" && root.state !== "stopping"
                             && root.state !== "teardown"
                    onClicked: {
                        if (root.state === "recording") {
                            root.stopRecording(true)
                        } else if (root.state === "preview") {
                            if (root.nativeCapture) {
                                root.node.chatVideoRecorder.finish(true)
                            } else {
                                const path = root.pendingPath
                                root.pendingPath = ""
                                root.beginTeardownThenClose(false)
                                root.node.sendCapturedMedia(
                                            path, "video/mp4",
                                            "circle-message.mp4", "circle")
                            }
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
                            if (root.nativeCapture) {
                                root.node.chatVideoRecorder.discardRecording()
                                root.pendingPath = ""
                                return
                            }
                            root.resetCapture(true)
                            return
                        }
                        if (root.nativeCapture) {
                            root.node.chatVideoRecorder.switchCamera()
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
                        root.restartCamera(next)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: {
                    if (root.state === "starting") return qsTr("Запуск камеры…")
                    if (root.state === "stopping") return qsTr("Сохранение записи…")
                    if (root.state === "teardown") return qsTr("Закрытие камеры…")
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

    Timer {
        id: cameraRestartTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (root.pendingCameraDevice) {
                camera.cameraDevice = root.pendingCameraDevice
                root.pendingCameraDevice = null
            }
            if (root.opened && root.state !== "teardown")
                root.cameraWanted = true
        }
    }
}
