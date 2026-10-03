pragma Singleton
import QtQuick
import QtQuick.Controls

QtObject {
    readonly property bool darkMode: Application.styleHints.colorScheme !== Qt.ColorScheme.Light
    readonly property color canvas: darkMode ? "#10100E" : "#F2F0EA"
    readonly property color surface: darkMode ? "#191915" : "#FBFAF6"
    readonly property color raised: darkMode ? "#25241E" : "#E7E3D9"
    readonly property color border: darkMode ? "#3B3930" : "#CFC8B8"
    readonly property color textPrimary: darkMode ? "#F2F0E8" : "#171712"
    readonly property color textSecondary: darkMode ? "#AAA79B" : "#666257"
    readonly property color accent: "#FF5A36"
    readonly property color keep: darkMode ? "#B9D27B" : "#52711F"
    readonly property color remove: darkMode ? "#FF7658" : "#BD3216"
    readonly property color warning: darkMode ? "#F2C15B" : "#8C6200"
    readonly property color cool: darkMode ? "#9DBCF6" : "#315EAF"
    readonly property string displayFont: "Geist"
    readonly property string bodyFont: "Geist"
    readonly property string monoFont: "Geist Mono"
    readonly property int immediateDuration: 90
    readonly property int controlDuration: 180
    readonly property int expressiveDuration: 420
    readonly property int cinematicDuration: 700
}
