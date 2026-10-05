import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import PhoneMemorySlider

Item {
    id: root
    property var devices: []
    property string statusText: ""
    signal refreshRequested()
    signal deviceRequested(int index)
    signal folderRequested(url folder)

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose a mounted phone or DCIM folder")
        onAccepted: root.folderRequested(selectedFolder)
    }

    RowLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 96, 1080)
        spacing: 72

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 18
            Label {
                text: qsTr("Your gallery.\nYour machine.")
                color: Theme.textPrimary
                font.family: Theme.displayFont
                font.pixelSize: 64
                font.weight: Font.DemiBold
                lineHeight: 0.96
            }
            Label {
                Layout.maximumWidth: 560
                text: qsTr("Plug in an unlocked phone. Photos are read over USB, learned from locally, and never uploaded.")
                color: Theme.textSecondary
                font.pixelSize: 19
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: [qsTr("ZERO EGRESS"), qsTr("AES-256-GCM VAULT"), qsTr("NO PHONE APP")]
                    delegate: Label {
                        required property string modelData
                        text: modelData
                        color: Theme.textSecondary
                        font.family: Theme.monoFont
                        font.pixelSize: 11
                        font.letterSpacing: 1.1
                        leftPadding: 11; rightPadding: 11; topPadding: 7; bottomPadding: 7
                        background: Rectangle { color: Theme.raised; radius: 15; border.color: Theme.border }
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 420
            Layout.preferredHeight: 500
            radius: 24
            color: Theme.surface
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 30
                spacing: 14
                Label { text: qsTr("Cable sources"); color: Theme.textPrimary; font.pixelSize: 28; font.weight: Font.DemiBold }
                Label { text: root.statusText; color: Theme.textSecondary; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                ListView {
                    id: deviceList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8
                    clip: true
                    model: root.devices
                    delegate: Button {
                        required property int index
                        required property var modelData
                        width: deviceList.width
                        height: 72
                        onClicked: root.deviceRequested(index)
                        Accessible.name: qsTr("Scan %1").arg(modelData.name)
                        contentItem: Column {
                            leftPadding: 8
                            spacing: 4
                            Label { text: modelData.name; color: Theme.textPrimary; font.weight: Font.DemiBold; font.pixelSize: 16 }
                            Label { text: modelData.detail; color: Theme.textSecondary; font.pixelSize: 12 }
                        }
                        background: Rectangle { radius: 12; color: parent.hovered ? Theme.raised : "transparent"; border.color: Theme.border }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: deviceList.count === 0
                        text: qsTr("Unlock your phone and approve this computer")
                        color: Theme.textSecondary
                        wrapMode: Text.WordWrap
                        width: parent.width - 32
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button { text: qsTr("Rescan USB"); onClicked: root.refreshRequested(); Accessible.name: text }
                    Button { Layout.fillWidth: true; text: qsTr("Choose DCIM folder"); highlighted: true; onClicked: folderDialog.open(); Accessible.name: text }
                }
            }
        }
    }
}
