import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PhoneMemorySlider

Item {
    id: root
    property bool reducedMotion: false
    signal connectRequested(string address, string code)

    function connectPhone() {
        if (addressField.text.length > 0 && codeField.text.length === 6)
            connectRequested(addressField.text, codeField.text)
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 72
        anchors.rightMargin: 72
        anchors.topMargin: 54
        anchors.bottomMargin: 54
        spacing: 72

        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: 570
            Layout.alignment: Qt.AlignVCenter
            spacing: 22

            Label {
                text: qsTr("Your camera roll,\nwith an exit.")
                color: Theme.textPrimary
                font.family: Theme.displayFont
                font.pixelSize: 68
                font.weight: Font.DemiBold
                font.letterSpacing: -2.2
                lineHeight: 0.96
            }
            Label {
                Layout.maximumWidth: 520
                wrapMode: Text.WordWrap
                text: qsTr("Use USB tethering or the same private network. Your photos stay between this computer and your phone—no account, cloud, or upload.")
                color: Theme.textSecondary
                font.family: Theme.bodyFont
                font.pixelSize: 18
                lineHeight: 1.45
            }

            RowLayout {
                spacing: 12
                Repeater {
                    model: [qsTr("Local only"), qsTr("Recoverable trash"), qsTr("Learns on-device")]
                    delegate: Label {
                        required property string modelData
                        text: modelData
                        color: Theme.textSecondary
                        font.pixelSize: 13
                        leftPadding: 12; rightPadding: 12; topPadding: 7; bottomPadding: 7
                        background: Rectangle { color: Theme.raised; radius: 14; border.color: Theme.border }
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 420
            Layout.preferredHeight: 520
            Layout.alignment: Qt.AlignVCenter
            radius: 22
            color: Theme.surface
            border.color: Theme.border
            border.width: 1

            Rectangle {
                width: 92; height: 8; radius: 4
                color: Theme.accent
                anchors.top: parent.top; anchors.right: parent.right
                anchors.topMargin: 28; anchors.rightMargin: 28
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 34
                spacing: 18

                Label { text: qsTr("Pair your phone"); color: Theme.textPrimary; font.family: Theme.displayFont; font.pixelSize: 30; font.weight: Font.DemiBold }
                Label { text: qsTr("1  Open the companion app and tap “Start local pairing”."); color: Theme.textSecondary; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 15 }

                Label { text: qsTr("2  Enter the local address"); color: Theme.textPrimary; font.pixelSize: 14; font.weight: Font.DemiBold }
                TextField {
                    id: addressField
                    Layout.fillWidth: true
                    placeholderText: qsTr("192.168.1.24:41820")
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textSecondary
                    font.family: Theme.bodyFont
                    font.pixelSize: 16
                    Accessible.name: qsTr("Phone local address")
                    onAccepted: codeField.forceActiveFocus()
                    background: Rectangle { implicitHeight: 52; radius: 10; color: Theme.raised; border.color: addressField.activeFocus ? Theme.accent : Theme.border; border.width: addressField.activeFocus ? 2 : 1 }
                }

                Label { text: qsTr("3  Match the six-digit code"); color: Theme.textPrimary; font.pixelSize: 14; font.weight: Font.DemiBold }
                TextField {
                    id: codeField
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhDigitsOnly
                    maximumLength: 6
                    horizontalAlignment: Text.AlignHCenter
                    placeholderText: qsTr("000 000")
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textSecondary
                    font.family: Theme.displayFont
                    font.pixelSize: 28
                    font.letterSpacing: 8
                    Accessible.name: qsTr("Six-digit pairing code")
                    onAccepted: root.connectPhone()
                    background: Rectangle { implicitHeight: 62; radius: 10; color: Theme.raised; border.color: codeField.activeFocus ? Theme.accent : Theme.border; border.width: codeField.activeFocus ? 2 : 1 }
                }

                Item { Layout.fillHeight: true }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("Verify and connect")
                    enabled: addressField.text.length > 0 && codeField.text.length === 6
                    onClicked: root.connectPhone()
                    Accessible.name: text
                    contentItem: Label { text: parent.text; color: "#171712"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.weight: Font.DemiBold; font.pixelSize: 16 }
                    background: Rectangle { implicitHeight: 54; radius: 10; color: parent.enabled ? Theme.accent : Theme.border; opacity: parent.down ? 0.78 : 1 }
                }
            }
        }
    }
}
