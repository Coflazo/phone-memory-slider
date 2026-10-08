// generated from design/tokens.json, do not edit; run python tools/tokens_to_qml.py
pragma Singleton
import QtQuick

QtObject {
    readonly property bool dark: Qt.styleHints.colorScheme !== Qt.ColorScheme.Light

    readonly property color canvas: dark ? "#10100E" : "#F2F0EA"
    readonly property color surface: dark ? "#191915" : "#FBFAF6"
    readonly property color raised: dark ? "#25241E" : "#E7E3D9"
    readonly property color border: dark ? "#3B3930" : "#CFC8B8"
    readonly property color textPrimary: dark ? "#F2F0E8" : "#171712"
    readonly property color textSecondary: dark ? "#AAA79B" : "#666257"
    readonly property color brand: dark ? "#FF5A36" : "#E2461F"
    readonly property color keep: dark ? "#B9D27B" : "#4A661B"
    readonly property color remove: dark ? "#FF7658" : "#BD3216"
    readonly property color warning: dark ? "#F2C15B" : "#7F5800"
    readonly property color info: dark ? "#9DBCF6" : "#315EAF"

    readonly property int spaceXxs: 4
    readonly property int spaceXs: 8
    readonly property int spaceSm: 12
    readonly property int spaceMd: 16
    readonly property int spaceLg: 24
    readonly property int spaceXl: 32
    readonly property int spaceXxl: 48

    readonly property int radiusControl: 6
    readonly property int radiusPanel: 10
    readonly property int radiusCard: 16
    readonly property int radiusChip: 6

    readonly property int motionDeckSettle: 260
    readonly property int motionDeckThrow: 220
    readonly property int motionDeckRise: 220
    readonly property int motionChrome: 150
    readonly property int motionStrike: 220
    readonly property int motionThumbFade: 160
    readonly property int motionSkeletonPeriod: 1200
    readonly property int motionReduced: 120

    readonly property real deckNextScale: 0.95
    readonly property int deckNextOffset: 16

    readonly property string fontSans: "Geist"
    readonly property string fontMono: "Geist Mono"
    readonly property int fontSizeLabel: 12
    readonly property int fontSizeSecondary: 13
    readonly property int fontSizeBody: 14
    readonly property int fontSizeTitleSmall: 20
    readonly property int fontSizeTitle: 24
    readonly property int fontSizeTitleLarge: 32
    readonly property int fontWeightTitle: 600
    readonly property real lineHeightBody: 1.45

    // Deprecated v1 names, removed in Plan C Task 11.
    readonly property bool darkMode: dark
    readonly property color accent: brand
    readonly property color cool: info
    readonly property string displayFont: fontSans
    readonly property string bodyFont: fontSans
    readonly property string monoFont: fontMono
    readonly property int controlRadius: radiusControl
    readonly property int immediateDuration: motionReduced
    readonly property int controlDuration: motionChrome
    readonly property int expressiveDuration: motionDeckSettle
}
