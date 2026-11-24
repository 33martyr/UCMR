pragma Singleton
import QtQuick

// Every colour, size and font of the launcher. Change the look here.
QtObject {
    // near black with the Collection's yellow-gold accent
    // panels are nearly opaque grey so text stays readable over the background artwork
    readonly property color bg: "#1d1d21"
    readonly property color panel: "#e62a2a2f"
    readonly property color card: "#ea2e2e33"
    readonly property color cardLine: "#3a3a42"
    readonly property color field: "#f0393940"
    readonly property color fieldHi: "#f544444c"
    readonly property color line: "#45454e"
    readonly property color text: "#f2efe8"
    readonly property color muted: "#a3a09a"
    readonly property color dim: "#6c6a70"
    readonly property color gold: "#e8b23e"
    readonly property color goldHi: "#f7c75e"
    readonly property color ink: "#16110a"
    readonly property color red: "#c4553f"

    readonly property int radiusCard: 16
    readonly property int radiusField: 10
    readonly property int gutter: 40
    readonly property int windowWidth: 1180

    readonly property string serif: "Georgia"
    readonly property string sans: "Segoe UI"
    readonly property string sansSemi: "Segoe UI Semibold"

    readonly property int fontSmall: 12
    readonly property int fontBody: 13
    readonly property int fontRowTitle: 14
    readonly property int fontSection: 16

    // the Tk launcher's colours (labels keep their meaning: gold notes, dim paths, red errors)
    function mapColor(c) {
        const k = String(c).toLowerCase()
        if (k === "#e8b23e" || k === "#f7c75e") return gold
        if (k === "#a3a09a") return muted
        if (k === "#6c6a70") return dim
        if (k === "#c4553f") return red
        if (k === "#16110a") return ink
        if (k === "" || k === "#f2efe8") return text
        return c
    }
}
