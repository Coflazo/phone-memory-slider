import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PhoneMemorySlider

Item {
    id: root
    property string phoneModel: ""
    property string statusText: ""
    property var qrModules: []
    property int qrSize: 0
    property bool pairingReady: false
    property bool phoneConnected: false
    property bool galleryPermissionReady: false
    property bool catalogReady: false
    property bool reducedMotion: false
    signal modelEdited(string model)
    signal startRequested()
    signal fallbackRequested()

    readonly property var steps: [
        { "label": qsTr("Phone model confirmed"), "complete": phoneModel.length > 0 },
        { "label": qsTr("Private Bluetooth service ready"), "complete": pairingReady },
        { "label": qsTr("QR scanned and phone connected"), "complete": phoneConnected },
        { "label": qsTr("Full gallery access allowed"), "complete": galleryPermissionReady },
        { "label": qsTr("Gallery catalog received"), "complete": catalogReady }
    ]

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 64
        anchors.rightMargin: 64
        anchors.topMargin: 44
        anchors.bottomMargin: 44
        spacing: 64

        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: 590
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            Label {
                Layout.fillWidth: true
                text: root.pairingReady ? qsTr("Scan once. Bluetooth takes it from here.")
                                        : qsTr("What phone are you connecting?")
                color: Theme.textPrimary
                font.family: Theme.displayFont
                font.pixelSize: 52
                font.weight: Font.DemiBold
                font.letterSpacing: -1.7
                wrapMode: Text.WordWrap
                lineHeight: 1.0
            }
            Label {
                Layout.topMargin: 18
                Layout.fillWidth: true
                text: qsTr("The QR carries a one-use session key and this computer’s Bluetooth address. No Wi-Fi, account, cloud, or Internet permission is involved.")
                color: Theme.textSecondary
                font.family: Theme.bodyFont
                font.pixelSize: 17
                lineHeight: 1.45
                wrapMode: Text.WordWrap
            }

            TextField {
                id: modelField
                Layout.topMargin: 30
                Layout.fillWidth: true
                visible: !root.pairingReady
                text: root.phoneModel
                placeholderText: qsTr("Phone model, for example Pixel 9 or Galaxy S25")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textSecondary
                font.family: Theme.bodyFont
                font.pixelSize: 16
                Accessible.name: qsTr("Phone model")
                onTextEdited: root.modelEdited(text)
                onAccepted: {
                    root.modelEdited(text)
                    if (text.trim().length > 0)
                        root.startRequested()
                }
                background: Rectangle {
                    implicitHeight: 56
                    radius: Theme.controlRadius
                    color: Theme.surface
                    border.color: modelField.activeFocus ? Theme.accent : Theme.border
                    border.width: modelField.activeFocus ? 2 : 1
                    Behavior on border.color { ColorAnimation { duration: Theme.controlDuration } }
                }
            }

            Button {
                Layout.topMargin: 12
                Layout.fillWidth: true
                visible: !root.pairingReady
                enabled: modelField.text.trim().length > 0
                text: qsTr("Create private pairing code")
                onClicked: {
                    root.modelEdited(modelField.text)
                    root.startRequested()
                }
                Accessible.name: text
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "#171712" : Theme.textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: Theme.bodyFont
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                background: Rectangle {
                    implicitHeight: 54
                    radius: Theme.controlRadius
                    color: parent.enabled ? Theme.accent : Theme.border
                    scale: parent.down && !root.reducedMotion ? 0.98 : 1
                    Behavior on scale { NumberAnimation { duration: Theme.immediateDuration; easing.type: Easing.OutExpo } }
                }
            }

            ColumnLayout {
                Layout.topMargin: 30
                Layout.fillWidth: true
                spacing: 14
                Repeater {
                    model: root.steps
                    delegate: RowLayout {
                        id: stepRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 14
                        property bool complete: modelData.complete

                        Rectangle {
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            radius: 16
                            color: stepRow.complete ? Theme.keep : Theme.raised
                            border.color: stepRow.complete ? Theme.keep : Theme.border
                            Label {
                                anchors.centerIn: parent
                                text: stepRow.index + 1
                                color: stepRow.complete ? Theme.canvas : Theme.textSecondary
                                font.family: Theme.monoFont
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            Behavior on color { ColorAnimation { duration: Theme.controlDuration } }
                            Behavior on border.color { ColorAnimation { duration: Theme.controlDuration } }
                            SequentialAnimation on scale {
                                running: stepRow.complete && !root.reducedMotion
                                NumberAnimation { to: 1.12; duration: 100; easing.type: Easing.OutExpo }
                                NumberAnimation { to: 1.0; duration: 160; easing.type: Easing.OutExpo }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: stepRow.modelData.label
                            color: stepRow.complete ? Theme.textSecondary : Theme.textPrimary
                            font.family: Theme.bodyFont
                            font.pixelSize: 16
                            font.strikeout: stepRow.complete
                            opacity: stepRow.complete ? 0.72 : 1
                            Behavior on opacity { NumberAnimation { duration: Theme.controlDuration; easing.type: Easing.OutExpo } }
                        }
                    }
                }
            }

            Label {
                Layout.topMargin: 24
                Layout.fillWidth: true
                text: root.statusText
                color: Theme.textSecondary
                font.family: Theme.monoFont
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Button {
                Layout.topMargin: 10
                text: qsTr("Use a cable or local folder instead")
                flat: true
                onClicked: root.fallbackRequested()
                Accessible.name: text
            }
        }

        Rectangle {
            Layout.preferredWidth: 420
            Layout.preferredHeight: 520
            Layout.alignment: Qt.AlignVCenter
            radius: 20
            color: Theme.surface
            border.color: Theme.border
            border.width: 1
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 30
                spacing: 18

                Label {
                    text: root.pairingReady ? qsTr("Open the companion app and scan") : qsTr("Pairing code will appear here")
                    color: Theme.textPrimary
                    font.family: Theme.displayFont
                    font.pixelSize: 25
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Rectangle {
                        id: qrPaper
                        width: Math.min(parent.width, parent.height)
                        height: width
                        anchors.centerIn: parent
                        radius: 12
                        color: "#FFFFFF"
                        visible: root.pairingReady
                        opacity: visible ? 1 : 0
                        scale: visible ? 1 : (root.reducedMotion ? 1 : 0.96)
                        Behavior on opacity { NumberAnimation { duration: root.reducedMotion ? 100 : 220; easing.type: Easing.OutExpo } }
                        Behavior on scale { NumberAnimation { duration: root.reducedMotion ? 100 : 360; easing.type: Easing.OutExpo } }

                        Canvas {
                            id: qrCanvas
                            anchors.fill: parent
                            anchors.margins: 14
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.fillStyle = "#FFFFFF"
                                ctx.fillRect(0, 0, width, height)
                                if (root.qrSize <= 0 || root.qrModules.length !== root.qrSize * root.qrSize)
                                    return
                                var quiet = 4
                                var module = Math.floor(Math.min(width, height) / (root.qrSize + quiet * 2))
                                var offsetX = Math.floor((width - module * root.qrSize) / 2)
                                var offsetY = Math.floor((height - module * root.qrSize) / 2)
                                ctx.fillStyle = "#10100E"
                                for (var y = 0; y < root.qrSize; ++y) {
                                    for (var x = 0; x < root.qrSize; ++x) {
                                        if (root.qrModules[y * root.qrSize + x])
                                            ctx.fillRect(offsetX + x * module, offsetY + y * module, module, module)
                                    }
                                }
                            }
                            Connections {
                                target: root
                                function onQrModulesChanged() { qrCanvas.requestPaint() }
                                function onQrSizeChanged() { qrCanvas.requestPaint() }
                            }
                        }
                    }

                    Column {
                        anchors.centerIn: parent
                        visible: !root.pairingReady
                        spacing: 12
                        Rectangle {
                            width: 54; height: 54; radius: 12
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: Theme.raised
                            border.color: Theme.border
                            Rectangle { width: 20; height: 20; anchors.centerIn: parent; color: Theme.accent; radius: 4; rotation: 8 }
                        }
                        Label {
                            width: 260
                            text: qsTr("Enter the model first. The computer will then open a one-use local session.")
                            color: Theme.textSecondary
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            font.pixelSize: 14
                            lineHeight: 1.35
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.pairingReady
                    text: qsTr("Android will show one system pairing confirmation. After that, catalog and previews move over Bluetooth automatically.")
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    lineHeight: 1.35
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
