import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PhoneMemorySlider

Item {
    id: root
    property real progress: 0
    property string statusText: ""
    property string deviceName: ""
    property bool reducedMotion: false
    signal cancelRequested()

    RowLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 120, 980)
        spacing: 72

        Item {
            Layout.preferredWidth: 330
            Layout.preferredHeight: 330

            Repeater {
                model: 3
                delegate: Rectangle {
                    required property int index
                    anchors.centerIn: parent
                    width: 286 - index * 54
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.width: index === 0 ? 2 : 1
                    border.color: index === 0 ? Theme.accent : Theme.border
                    opacity: 0.95 - index * 0.2
                    transform: Rotation { origin.x: width / 2; origin.y: height / 2; angle: root.reducedMotion ? 0 : orbit.rotation * (index % 2 ? -1 : 1) }
                    Rectangle { width: index === 0 ? 18 : 10; height: width; radius: width / 2; color: index === 0 ? Theme.accent : Theme.cool; anchors.horizontalCenter: parent.horizontalCenter; y: -height / 2 }
                }
            }

            NumberAnimation {
                id: orbit
                target: orbit
                property: "rotation"
                property real rotation: 0
                from: 0; to: 360
                duration: 3600
                loops: Animation.Infinite
                running: !root.reducedMotion && root.visible
            }

            Column {
                anchors.centerIn: parent
                spacing: 4
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: Math.round(root.progress * 100) + "%"; color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 52; font.weight: Font.DemiBold }
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("LOCAL"); color: Theme.textSecondary; font.pixelSize: 11; font.letterSpacing: 2 }
            }
        }

        ColumnLayout {
            Layout.preferredWidth: 490
            spacing: 16
            Label { text: qsTr("Reading %1").arg(root.deviceName || qsTr("your phone")); color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 44; font.weight: Font.DemiBold }
            Label { Layout.fillWidth: true; text: root.statusText; color: Theme.textSecondary; font.pixelSize: 17; wrapMode: Text.WordWrap; lineHeight: 1.4 }
            ProgressBar {
                Layout.fillWidth: true
                from: 0; to: 1; value: root.progress
                Accessible.name: qsTr("Gallery analysis progress")
                background: Rectangle { implicitHeight: 6; radius: 3; color: Theme.raised }
                contentItem: Item { implicitHeight: 6; Rectangle { width: parent.width * root.progress; height: parent.height; radius: 3; color: Theme.accent } }
            }
            Label { text: qsTr("The visual model and every decision remain on this computer."); color: Theme.textSecondary; font.pixelSize: 13 }
            Button { text: qsTr("Cancel scan"); flat: true; onClicked: root.cancelRequested(); Accessible.name: text }
        }
    }
}
