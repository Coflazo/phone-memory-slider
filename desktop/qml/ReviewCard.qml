import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import PhoneMemorySlider

Item {
    id: root
    width: 620
    height: 650
    property var model
    property bool reducedMotion: false
    signal deleteRequested()
    signal keepRequested()

    function settle(action) {
        if (reducedMotion) {
            action()
            card.x = 0
            card.opacity = 1
            return
        }
        exitAction = action
        exitAnimation.to = card.x < 0 ? -root.width * 1.4 : root.width * 1.4
        exitAnimation.start()
    }
    function reject() { card.x = -root.width * 0.34; settle(root.deleteRequested) }
    function keep() { card.x = root.width * 0.34; settle(root.keepRequested) }
    property var exitAction

    Rectangle {
        id: card
        width: 590
        height: 608
        x: 15
        y: 6
        radius: 16
        color: Theme.surface
        border.width: 1
        border.color: Theme.border
        clip: true
        transform: Rotation {
            origin.x: card.width / 2
            origin.y: card.height
            angle: Math.max(-6, Math.min(6, card.x / 55))
        }

        DragHandler {
            id: drag
            target: card
            yAxis.enabled: false
            xAxis.minimum: -root.width * 0.72
            xAxis.maximum: root.width * 0.72
            onActiveChanged: {
                if (!active) {
                    if (card.x < -root.width * 0.22) root.settle(root.deleteRequested)
                    else if (card.x > root.width * 0.22) root.settle(root.keepRequested)
                    else returnAnimation.start()
                }
            }
        }

        ListView {
            id: view
            anchors.fill: parent
            model: root.model
            interactive: false
            delegate: Item {
                width: view.width
                height: view.height

                Image {
                    anchors.fill: parent
                    visible: mediaKind !== "video"
                    source: previewUrl
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }

                Rectangle {
                    anchors.fill: parent
                    visible: mediaKind === "video"
                    color: "#090907"
                }

                VideoOutput {
                    id: videoSurface
                    anchors.fill: parent
                    visible: mediaKind === "video" && player.source.toString().length > 0
                    fillMode: VideoOutput.PreserveAspectFit
                }

                AudioOutput { id: videoAudio; muted: true }
                MediaPlayer {
                    id: player
                    source: mediaKind === "video" ? previewUrl : ""
                    videoOutput: videoSurface
                    audioOutput: videoAudio
                    loops: MediaPlayer.Infinite
                    onSourceChanged: if (source.toString().length > 0) play()
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    visible: mediaKind === "video" && player.source.toString().length === 0
                    running: visible
                    Accessible.name: qsTr("Caching video locally")
                }

                Row {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 16
                    spacing: 8
                    visible: mediaKind === "video" && player.source.toString().length > 0
                    Button {
                        text: player.playbackState === MediaPlayer.PlayingState ? qsTr("Pause") : qsTr("Play")
                        onClicked: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
                        Accessible.name: text + qsTr(" video")
                    }
                    Button {
                        text: videoAudio.muted ? qsTr("Sound off") : qsTr("Sound on")
                        onClicked: videoAudio.muted = !videoAudio.muted
                        Accessible.name: videoAudio.muted ? qsTr("Unmute video") : qsTr("Mute video")
                    }
                }

                Shortcut {
                    enabled: mediaKind === "video" && player.source.toString().length > 0
                    sequence: "Space"
                    onActivated: player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 142
                    color: "#E60B0D10"
                }

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 22
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: reason
                            color: Theme.textPrimary
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            leftPadding: 10
                            rightPadding: 10
                            topPadding: 6
                            bottomPadding: 6
                            background: Rectangle { color: Theme.raised; radius: 8; border.color: Theme.border }
                        }
                        Item { Layout.fillWidth: true }
                        Label { text: storageText; color: Theme.textPrimary; font.pixelSize: 15; font.family: Theme.monoFont }
                    }
                    Label { text: displayName; color: Theme.textPrimary; font.pixelSize: 24; font.weight: Font.DemiBold }
                    Label {
                        text: mediaKind === "video" ? qsTr("Video stays local and plays from a temporary cache") : qsTr("Swipe left to queue, right to keep")
                        color: Theme.textSecondary
                        font.pixelSize: 14
                    }
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: card.x < 0 ? Theme.remove : Theme.keep
            opacity: Math.min(0.20, Math.abs(card.x) / root.width * 0.24)
        }
    }

    NumberAnimation {
        id: exitAnimation
        target: card
        property: "x"
        duration: root.reducedMotion ? 0 : Theme.controlDuration
        easing.type: Easing.OutExpo
        onFinished: {
            if (root.exitAction) root.exitAction()
            card.x = 15
            card.opacity = 1
        }
    }

    SpringAnimation {
        id: returnAnimation
        target: card
        property: "x"
        to: 15
        spring: 3.2
        damping: 0.30
        epsilon: 0.25
    }

    RowLayout {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        spacing: 14
        Button {
            text: qsTr("Queue for trash")
            onClicked: root.reject()
            Accessible.name: qsTr("Queue current item for trash")
        }
        Button {
            text: qsTr("Keep")
            highlighted: true
            onClicked: root.keep()
            Accessible.name: qsTr("Keep current item")
        }
    }
}
