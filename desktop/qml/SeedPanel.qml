import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import PhoneMemorySlider

Item {
    id: root
    property string statusText: ""
    property string coverageText: ""
    signal seedsChosen(var files)
    signal skipRequested()
    signal cancelRequested()

    FileDialog {
        id: keepDialog
        title: qsTr("Choose 20–50 photos you love")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp *.heic *.heif)")]
        onAccepted: root.seedsChosen(selectedFiles)
    }

    RowLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 120, 1000)
        spacing: 76
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 16
            Label { text: qsTr("Teach it your taste."); color: Theme.textPrimary; font.pixelSize: 54; font.weight: Font.DemiBold }
            Label { Layout.maximumWidth: 540; text: root.statusText; color: Theme.textSecondary; font.pixelSize: 18; wrapMode: Text.WordWrap; lineHeight: 1.4 }
            Label { Layout.maximumWidth: 540; text: qsTr("Visible Favorites folders are protected and learned automatically. If your cable connection hides album metadata, add a local export here. Only compact visual vectors are used during this session."); color: Theme.textSecondary; font.pixelSize: 14; wrapMode: Text.WordWrap; lineHeight: 1.35 }
            Label { text: root.coverageText; color: Theme.cool; font.family: Theme.monoFont; font.pixelSize: 12 }
            RowLayout {
                Button { text: qsTr("Choose keep photos"); highlighted: true; onClicked: keepDialog.open(); Accessible.name: text }
                Button { text: qsTr("Analyze now"); onClicked: root.skipRequested(); Accessible.name: text }
                Button { text: qsTr("Cancel"); flat: true; onClicked: root.cancelRequested(); Accessible.name: text }
            }
        }
        Rectangle {
            Layout.preferredWidth: 300
            Layout.preferredHeight: 380
            radius: 150
            color: Theme.raised
            border.color: Theme.border
            Repeater {
                model: 3
                delegate: Rectangle {
                    required property int index
                    anchors.centerIn: parent
                    width: 230 - index * 58
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: index === 0 ? Theme.accent : (index === 1 ? Theme.cool : Theme.keep)
                    opacity: 0.8
                    RotationAnimation on rotation {
                        from: index % 2 ? 360 : 0
                        to: index % 2 ? 0 : 360
                        duration: 7000 + index * 1800
                        loops: Animation.Infinite
                        running: root.visible
                    }
                }
            }
            Label { anchors.centerIn: parent; text: qsTr("YOU"); color: Theme.textPrimary; font.family: Theme.monoFont; font.pixelSize: 18; font.letterSpacing: 3 }
        }
    }
}
