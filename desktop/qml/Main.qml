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

    Shortcut { sequence: "Left"; onActivated: card.reject() }
    Shortcut { sequence: "Right"; onActivated: card.keep() }
    Shortcut { sequence: "Z"; onActivated: reviewDeck.undo() }

    header: ToolBar {
        height: 72
        background: Rectangle { color: Theme.surface; border.color: Theme.border }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            Label {
                text: qsTr("Phone Memory Slider")
                color: Theme.textPrimary
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("%1 remaining  ·  %2 queued").arg(reviewDeck.remaining).arg(reviewDeck.pendingBytesText)
                color: Theme.textSecondary
                font.pixelSize: 14
            }
            Button {
                text: qsTr("Undo")
                enabled: !reviewDeck.complete
                onClicked: reviewDeck.undo()
                Accessible.name: qsTr("Undo last review decision")
            }
        }
    }

    Item {
        anchors.fill: parent

        Rectangle {
            width: 590
            height: 608
            radius: 16
            color: Theme.raised
            opacity: 0.45
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 16
            scale: 0.95
        }

        ReviewCard {
            id: card
            anchors.centerIn: parent
            model: reviewDeck
            reducedMotion: window.reducedMotion
            onDeleteRequested: reviewDeck.deleteCurrent()
            onKeepRequested: reviewDeck.keepCurrent()
        }

        Column {
            visible: reviewDeck.complete
            anchors.centerIn: parent
            spacing: 12
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Review complete")
                color: Theme.textPrimary
                font.pixelSize: 32
                font.weight: Font.DemiBold
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("%1 is ready for phone confirmation.").arg(reviewDeck.pendingBytesText)
                color: Theme.textSecondary
                font.pixelSize: 16
            }
        }
    }
}

