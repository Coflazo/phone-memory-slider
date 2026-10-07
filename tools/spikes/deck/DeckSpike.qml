// Spike 7: swipe-deck frame pacing with 12 MP photos and 1080p video on one shared player.
// Run: .tools/Qt/6.8.3/msvc2022_64/bin/qml.exe tools/spikes/deck/DeckSpike.qml
// Prints p50/p95/p99 frame interval over 200 scripted flings, then quits.
import QtQuick
import QtQuick.Window
import QtMultimedia

Window {
    id: root
    width: 1280; height: 800; visible: true; color: "#111110"
    title: "Deck spike"

    readonly property int flings: 200
    readonly property var photos: ["photo_1.jpg", "photo_2.jpg", "photo_3.jpg", "photo_4.jpg", "photo_5.jpg", "photo_6.jpg"]
    readonly property var clips: ["clip_h264.mp4", "clip_hevc.mp4"]
    property int top: 0           // index of the card currently on top
    property int done: 0
    property var intervals: []
    property bool measuring: false

    function isVideo(i) { return i % 4 === 3 }
    function mediaFor(i) { return Qt.resolvedUrl("media/" + (isVideo(i) ? clips[(i / 4) % 2 | 0] : photos[i % photos.length])) }

    MediaPlayer {
        id: player
        videoOutput: videoOut
        loops: MediaPlayer.Infinite
        audioOutput: null
    }

    // One shared VideoOutput, reparented visually by position: it sits over the top card only.
    Repeater {
        id: deck
        model: 3
        delegate: Item {
            id: card
            required property int index
            readonly property int cardIndex: root.top + index
            width: 520; height: 680
            x: (root.width - width) / 2 + (index === 0 ? drift : 0)
            y: (root.height - height) / 2 + index * 14
            z: 3 - index
            scale: 1 - index * 0.04
            property real drift: 0
            rotation: drift / 40
            Rectangle { anchors.fill: parent; radius: 18; color: "#1d1d1b"; border.color: "#2c2c29" }
            Image {
                anchors.fill: parent; anchors.margins: 10
                asynchronous: true
                sourceSize: Qt.size(1040, 1360)
                fillMode: Image.PreserveAspectCrop
                source: root.isVideo(card.cardIndex) ? "" : root.mediaFor(card.cardIndex)
            }
            NumberAnimation on drift { id: fling; running: false; duration: 280; easing.type: Easing.OutCubic
                onFinished: { card.drift = 0; root.advance() } }
            function throwAway() { fling.from = 0; fling.to = (root.done % 2 ? -1 : 1) * 1400; fling.start() }
        }
    }

    VideoOutput {
        id: videoOut
        width: 500; height: 660
        x: (root.width - width) / 2; y: (root.height - height) / 2
        z: 10
        visible: root.isVideo(root.top)
        fillMode: VideoOutput.PreserveAspectCrop
    }

    function advance() {
        done += 1
        top += 1
        if (isVideo(top)) {
            // start playback a beat after the swipe settles, poster until then
            startDelay.restart()
        } else {
            player.stop()
        }
        if (done >= flings) finish(); else nextFling.restart()
    }

    Timer { id: startDelay; interval: 150; onTriggered: { player.source = root.mediaFor(root.top); player.play() } }
    Timer { id: nextFling; interval: 120; onTriggered: deck.itemAt(0).throwAway() }
    Timer { interval: 1500; running: true; onTriggered: { root.measuring = true; deck.itemAt(0).throwAway() } }

    FrameAnimation {
        running: root.measuring
        onTriggered: root.intervals.push(frameTime * 1000.0)
    }

    function finish() {
        measuring = false
        const a = intervals.slice(5).sort((x, y) => x - y)
        const q = p => a[Math.min(a.length - 1, Math.floor(p * a.length))].toFixed(2)
        console.log(`frames=${a.length} p50=${q(0.5)}ms p95=${q(0.95)}ms p99=${q(0.99)}ms max=${a[a.length - 1].toFixed(2)}ms`)
        Qt.quit()
    }
}
