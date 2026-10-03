import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PhoneMemorySlider

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    minimumWidth: 980
    minimumHeight: 680
    visible: true
    color: Theme.canvas
    title: qsTr("Phone Memory Slider")
    property bool reducedMotion: systemReducedMotion
    property int stageDuration: reducedMotion ? Theme.immediateDuration : Theme.expressiveDuration
    property bool scanning: appController.stage === appController.Pairing ||
                            appController.stage === appController.Syncing ||
                            appController.stage === appController.Analyzing

    Shortcut { enabled: appController.stage === appController.Review; sequence: "Left"; onActivated: card.reject() }
    Shortcut { enabled: appController.stage === appController.Review; sequence: "Right"; onActivated: card.keep() }
    Shortcut { enabled: appController.stage === appController.Review; sequence: "Z"; onActivated: reviewDeck.undo() }
    Shortcut { sequence: "Escape"; enabled: appController.canCancel; onActivated: appController.cancel() }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
        Repeater {
            model: 7
            Rectangle {
                required property int index
                x: 34 + index * (window.width - 68) / 6
                width: 1
                height: parent.height
                color: Theme.border
                opacity: 0.19
            }
        }
    }

    header: ToolBar {
        height: 70
        background: Rectangle { color: Theme.canvas; border.color: Theme.border }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 30
            anchors.rightMargin: 30
            spacing: 16
            Rectangle { width: 16; height: 16; radius: 3; color: Theme.accent; rotation: 8 }
            Label { text: qsTr("Phone Memory Slider"); color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 17; font.weight: Font.DemiBold }
            Label { visible: appController.deviceName.length > 0; text: appController.deviceName; color: Theme.textSecondary; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            Label {
                visible: appController.stage === appController.Review
                text: qsTr("%1 left  /  %2 queued").arg(reviewDeck.remaining).arg(reviewDeck.pendingBytesText)
                color: Theme.textSecondary
                font.pixelSize: 14
            }
            Button { visible: appController.stage === appController.Review; text: qsTr("Undo"); flat: true; enabled: reviewDeck.remaining > 0; onClicked: reviewDeck.undo(); Accessible.name: qsTr("Undo last review decision") }
            Button { visible: appController.canCancel; text: qsTr("Cancel"); flat: true; onClicked: appController.cancel(); Accessible.name: qsTr("Cancel phone operation") }
            Label {
                text: qsTr("LOCAL")
                color: Theme.keep
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 1.6
                leftPadding: 10; rightPadding: 10; topPadding: 6; bottomPadding: 6
                background: Rectangle { radius: 12; color: Theme.raised; border.color: Theme.border }
            }
        }
    }

    Item {
        anchors.fill: parent

        PairingPanel {
            anchors.fill: parent
            reducedMotion: window.reducedMotion
            enabled: opacity > 0.99
            opacity: appController.stage === appController.Welcome ? 1 : 0
            scale: appController.stage === appController.Welcome ? 1 : (window.reducedMotion ? 1 : 0.985)
            onConnectRequested: (address, code) => appController.startPairing(address, code)
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
        }

        ScanPanel {
            anchors.fill: parent
            reducedMotion: window.reducedMotion
            progress: appController.progress
            statusText: appController.statusText
            deviceName: appController.deviceName
            enabled: opacity > 0.99
            opacity: window.scanning ? 1 : 0
            scale: window.scanning ? 1 : (window.reducedMotion ? 1 : 1.025)
            onCancelRequested: appController.cancel()
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
        }

        Item {
            anchors.fill: parent
            enabled: opacity > 0.99
            opacity: appController.stage === appController.Review ? 1 : 0
            scale: appController.stage === appController.Review ? 1 : (window.reducedMotion ? 1 : 0.97)
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }

            Rectangle { width: 590; height: 608; radius: 18; color: Theme.raised; opacity: 0.42; anchors.centerIn: parent; anchors.verticalCenterOffset: 20; rotation: -2.2 }
            ReviewCard {
                id: card
                anchors.centerIn: parent
                model: reviewDeck
                reducedMotion: window.reducedMotion
                onDeleteRequested: reviewDeck.deleteCurrent()
                onKeepRequested: reviewDeck.keepCurrent()
            }
        }

        ReviewSummary {
            anchors.fill: parent
            pendingCount: reviewDeck.pendingCount
            pendingBytesText: reviewDeck.pendingBytesText
            confirming: appController.stage === appController.Confirming
            complete: appController.stage === appController.Complete
            statusText: appController.statusText
            enabled: opacity > 0.99
            opacity: appController.stage === appController.Summary || appController.stage === appController.Confirming || appController.stage === appController.Complete ? 1 : 0
            scale: opacity > 0 ? 1 : (window.reducedMotion ? 1 : 1.02)
            onBackRequested: appController.returnToReview()
            onConfirmRequested: appController.confirmTrash()
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
        }

        Rectangle {
            anchors.fill: parent
            visible: opacity > 0
            opacity: appController.stage === appController.Error ? 1 : 0
            color: "#B810100E"
            Behavior on opacity { NumberAnimation { duration: window.reducedMotion ? 80 : 180; easing.type: Easing.OutExpo } }
            Rectangle {
                width: 500; height: 286; radius: 18
                anchors.centerIn: parent
                color: Theme.surface; border.color: Theme.border
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 30; spacing: 14
                    Label { text: qsTr("The connection paused"); color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 30; font.weight: Font.DemiBold }
                    Label { Layout.fillWidth: true; Layout.fillHeight: true; text: appController.errorText; color: Theme.textSecondary; wrapMode: Text.WordWrap; font.pixelSize: 16; lineHeight: 1.4 }
                    RowLayout {
                        Button { text: qsTr("Back to pairing"); onClicked: appController.cancel(); Accessible.name: text }
                        Button { visible: reviewDeck.remaining > 0 || reviewDeck.pendingCount > 0; text: qsTr("Return to review"); highlighted: true; onClicked: appController.returnToReview(); Accessible.name: text }
                    }
                }
            }
        }
    }
}
