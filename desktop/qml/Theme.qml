pragma Singleton
import QtQuick
import QtQuick.Controls

QtObject {
    readonly property bool darkMode: Application.styleHints.colorScheme !== Qt.ColorScheme.Light
    readonly property color canvas: darkMode ? "#0B0D10" : "#F6F7F9"
    readonly property color surface: darkMode ? "#12161B" : "#FFFFFF"
    readonly property color raised: darkMode ? "#181D24" : "#EEF1F5"
    readonly property color border: darkMode ? "#2A313B" : "#D8DEE7"
    readonly property color textPrimary: darkMode ? "#F3F5F7" : "#15181D"
    readonly property color textSecondary: darkMode ? "#A9B0BA" : "#5E6672"
    readonly property color accent: darkMode ? "#6B7CFF" : "#4F63E8"
    readonly property color keep: darkMode ? "#46C987" : "#218A55"
    readonly property color remove: darkMode ? "#FF5C67" : "#C73544"
    readonly property color warning: darkMode ? "#F4B860" : "#9A6500"
    readonly property int immediateDuration: 90
    readonly property int controlDuration: 180
    readonly property int expressiveDuration: 420
    readonly property int cinematicDuration: 700
}
