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
    property bool working: appController.stage === AppController.Syncing ||
                           appController.stage === AppController.Analyzing ||
                           appController.stage === AppController.Recovering

    function demoSwipe(keep) {
        if (appController.stage === AppController.Review)
            keep ? card.keep() : card.reject()
    }

    Shortcut { enabled: appController.stage === AppController.Review; sequence: "Left"; onActivated: card.reject() }
    Shortcut { enabled: appController.stage === AppController.Review; sequence: "Right"; onActivated: card.keep() }
    Shortcut { enabled: appController.stage === AppController.Review; sequence: "Z"; onActivated: reviewDeck.undo() }
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
                opacity: 0.16
            }
        }
        Rectangle {
            width: 460; height: 460; radius: 230
            x: -170; y: -230
            color: Theme.accent
            opacity: Theme.darkMode ? 0.055 : 0.035
            SequentialAnimation on scale {
                loops: Animation.Infinite
                running: !window.reducedMotion
                NumberAnimation { to: 1.12; duration: 3600; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1.0; duration: 3600; easing.type: Easing.InOutSine }
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
            spacing: 14
            Rectangle { width: 16; height: 16; radius: 3; color: Theme.accent; rotation: 8 }
            Label { text: qsTr("Phone Memory Slider"); color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 17; font.weight: Font.DemiBold }
            Label { visible: appController.deviceName.length > 0; text: "· " + appController.deviceName; color: Theme.textSecondary; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            Label {
                visible: appController.stage === AppController.Review
                text: qsTr("%1 left  /  %2 queued").arg(reviewDeck.remaining).arg(reviewDeck.pendingBytesText)
                color: Theme.textSecondary
                font.family: Theme.monoFont
                font.pixelSize: 13
            }
            Button { visible: appController.stage === AppController.Review; text: qsTr("Undo"); flat: true; onClicked: reviewDeck.undo(); Accessible.name: qsTr("Undo last decision") }
            Button { visible: appController.canCancel; text: qsTr("Cancel"); flat: true; onClicked: appController.cancel(); Accessible.name: qsTr("Cancel local operation") }
            Label {
                text: qsTr("OFFLINE")
                color: Theme.keep
                font.family: Theme.monoFont
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 1.5
                leftPadding: 10; rightPadding: 10; topPadding: 6; bottomPadding: 6
                background: Rectangle { radius: 12; color: Theme.raised; border.color: Theme.border }
            }
        }
    }

    Item {
        anchors.fill: parent

        DevicePanel {
            anchors.fill: parent
            devices: appController.devices
            statusText: appController.statusText
            enabled: opacity > 0.99
            opacity: appController.stage === AppController.Welcome ? 1 : 0
            scale: appController.stage === AppController.Welcome ? 1 : (window.reducedMotion ? 1 : 0.975)
            onRefreshRequested: appController.refreshDevices()
            onDeviceRequested: index => appController.scanDevice(index)
            onFolderRequested: folder => appController.scanFolder(folder)
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
        }

        SeedPanel {
            anchors.fill: parent
            statusText: appController.statusText
            coverageText: appController.coverageText
            enabled: opacity > 0.99
            opacity: appController.stage === AppController.Seeding ? 1 : 0
            scale: appController.stage === AppController.Seeding ? 1 : (window.reducedMotion ? 1 : 1.025)
            onSeedsChosen: files => appController.analyzeWithSeeds(files)
            onSkipRequested: appController.analyzeWithoutSeeds()
            onCancelRequested: appController.cancel()
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
            opacity: window.working ? 1 : 0
            scale: window.working ? 1 : (window.reducedMotion ? 1 : 1.025)
            onCancelRequested: appController.cancel()
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
        }

        Item {
            anchors.fill: parent
            enabled: opacity > 0.99
            opacity: appController.stage === AppController.Review ? 1 : 0
            scale: appController.stage === AppController.Review ? 1 : (window.reducedMotion ? 1 : 0.965)
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutBack } }

            Rectangle { width: 590; height: 608; radius: 18; color: Theme.raised; opacity: 0.38; anchors.centerIn: parent; anchors.verticalCenterOffset: 22; rotation: -2.3 }
            Rectangle { width: 590; height: 608; radius: 18; color: Theme.raised; opacity: 0.24; anchors.centerIn: parent; anchors.verticalCenterOffset: 36; rotation: 2.1 }
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
            confirming: appController.stage === AppController.Recovering
            complete: appController.stage === AppController.Complete
            statusText: appController.statusText
            vaultPath: appController.vaultPath
            enabled: opacity > 0.99
            opacity: appController.stage === AppController.Summary || appController.stage === AppController.Complete ? 1 : 0
            scale: opacity > 0 ? 1 : (window.reducedMotion ? 1 : 1.02)
            onBackRequested: appController.returnToReview()
            onConfirmRequested: appController.confirmDelete()
            Behavior on opacity { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
            Behavior on scale { NumberAnimation { duration: window.stageDuration; easing.type: Easing.OutExpo } }
        }

        Rectangle {
            anchors.fill: parent
            visible: opacity > 0
            opacity: appController.stage === AppController.Error ? 1 : 0
            color: "#B810100E"
            Behavior on opacity { NumberAnimation { duration: window.reducedMotion ? 80 : 180; easing.type: Easing.OutExpo } }
            Rectangle {
                width: 520; height: 300; radius: 20
                anchors.centerIn: parent
                color: Theme.surface; border.color: Theme.border
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 30; spacing: 14
                    Label { text: qsTr("The operation stopped safely"); color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 30; font.weight: Font.DemiBold }
                    Label { Layout.fillWidth: true; Layout.fillHeight: true; text: appController.errorText; color: Theme.textSecondary; wrapMode: Text.WordWrap; font.pixelSize: 16; lineHeight: 1.4 }
                    RowLayout {
                        Button { text: qsTr("Start over"); onClicked: appController.cancel(); Accessible.name: text }
                        Button { visible: reviewDeck.remaining > 0 || reviewDeck.pendingCount > 0; text: qsTr("Return to review"); highlighted: true; onClicked: appController.returnToReview(); Accessible.name: text }
                    }
                }
            }
        }
    }
}
