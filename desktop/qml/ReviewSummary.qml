import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PhoneMemorySlider

Item {
    id: root
    property int pendingCount: 0
    property string pendingBytesText: "0 MB"
    property bool confirming: false
    property bool complete: false
    property string statusText: ""
    property string vaultPath: ""
    signal backRequested()
    signal confirmRequested()

    RowLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 140, 1000)
        spacing: 82

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 16
            Label {
                text: root.complete ? qsTr("Back in control.") : (root.confirming ? qsTr("Building your safety net.") : qsTr("One last look."))
                color: Theme.textPrimary
                font.family: Theme.displayFont
                font.pixelSize: 58
                font.weight: Font.DemiBold
                font.letterSpacing: -1.5
            }
            Label {
                Layout.maximumWidth: 520
                text: root.complete || root.confirming ? root.statusText : qsTr("Nothing has moved yet. Each selected file will be copied into the encrypted vault and SHA-256 verified before the original is removed.")
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
                font.pixelSize: 18
                lineHeight: 1.45
            }
            Label {
                visible: root.complete
                Layout.maximumWidth: 520
                text: qsTr("Recovery vault: %1").arg(root.vaultPath)
                color: Theme.textSecondary
                wrapMode: Text.WrapAnywhere
                font.family: Theme.monoFont
                font.pixelSize: 12
            }
            RowLayout {
                spacing: 12
                Button { visible: !root.confirming && !root.complete; text: qsTr("Back to review"); onClicked: root.backRequested(); Accessible.name: text }
                Button { visible: !root.confirming && !root.complete; text: qsTr("Encrypt, verify & remove"); highlighted: true; onClicked: root.confirmRequested(); Accessible.name: text }
                BusyIndicator { visible: root.confirming; running: visible; Accessible.name: qsTr("Creating encrypted recovery copies") }
            }
        }

        Rectangle {
            Layout.preferredWidth: 340
            Layout.preferredHeight: 390
            radius: 18
            color: Theme.surface
            border.color: Theme.border
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 30; spacing: 8
                Label { text: root.pendingCount; color: Theme.accent; font.family: Theme.displayFont; font.pixelSize: 92; font.weight: Font.Bold }
                Label { text: qsTr("items queued"); color: Theme.textPrimary; font.pixelSize: 22; font.weight: Font.DemiBold }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; Layout.topMargin: 22; Layout.bottomMargin: 22 }
                Label { text: root.pendingBytesText; color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 38; font.weight: Font.DemiBold }
                Label { text: qsTr("potential space back"); color: Theme.textSecondary; font.pixelSize: 14 }
                Item { Layout.fillHeight: true }
                Label { text: qsTr("Encrypted recovery copy first"); color: Theme.keep; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
    }
}
