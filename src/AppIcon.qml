import QtQuick
import QtQuick.Shapes

Item {
    id: icon
    required property string name
    property color color: "black"
    readonly property bool outlined: ["open", "save", "overview", "visual", "markdown", "code", "check", "file", "history", "export", "bold", "italic", "underline", "headline", "comment", "media-add", "adjust", "chevron-down"].indexOf(name) >= 0
    readonly property real canvasSize: name === "open" || name === "save" ? 16 : 24
    implicitWidth: 24; implicitHeight: 24
    Shape {
        width: icon.canvasSize; height: icon.canvasSize; anchors.centerIn: parent
        scale: Math.min(icon.width, icon.height) / icon.canvasSize
        antialiasing: true
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            fillColor: icon.outlined ? "transparent" : icon.color
            strokeColor: icon.outlined ? icon.color : "transparent"
            strokeWidth: icon.outlined ? (icon.canvasSize === 16 ? 1.4 : icon.name === "bold" ? 2.2 : 1.8) : 0
            capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
            fillRule: ShapePath.OddEvenFill
            PathSvg {
                // Theme: Omarchy.org PaletteIcon. Open/save: Omawrite FooterIconButton.
                path: icon.name === "theme" ? "M2 12C2 6.71457 6.51697 2.5 12 2.5C17.483 2.5 22 6.71457 22 12C22 13.8878 21.4937 15.1519 20.4345 15.8123C19.4491 16.4266 18.2002 16.3605 17.1496 16.2236C16.7813 16.1757 16.397 16.1121 16.0307 16.0515C15.8617 16.0235 15.6965 15.9962 15.5384 15.9713C15.0184 15.8895 14.5499 15.8295 14.1335 15.8234C13.298 15.8112 12.8925 16.0091 12.671 16.4526C12.539 16.7171 12.5395 17.0363 12.6858 17.4705C12.8149 17.8538 13.0214 18.2277 13.2435 18.63C13.2828 18.7012 13.3226 18.7734 13.3626 18.8465C13.4857 19.0721 13.6169 19.3217 13.7077 19.5624C13.7926 19.7875 13.8875 20.117 13.8161 20.466C13.7303 20.8858 13.4434 21.1713 13.0891 21.3241C12.7768 21.4588 12.3992 21.5 12 21.5C6.51697 21.5 2 17.2854 2 12ZM10.25 6.25C9.42157 6.25 8.75 6.92157 8.75 7.75C8.75 8.57843 9.42157 9.25 10.25 9.25C11.0784 9.25 11.75 8.57843 11.75 7.75C11.75 6.92157 11.0784 6.25 10.25 6.25ZM7.25 10.5C6.42157 10.5 5.75 11.1716 5.75 12C5.75 12.8284 6.42157 13.5 7.25 13.5C8.07843 13.5 8.75 12.8284 8.75 12C8.75 11.1716 8.07843 10.5 7.25 10.5ZM15.25 7.75C14.4216 7.75 13.75 8.42157 13.75 9.25C13.75 10.0784 14.4216 10.75 15.25 10.75C16.0784 10.75 16.75 10.0784 16.75 9.25C16.75 8.42157 16.0784 7.75 15.25 7.75Z"
                    : icon.name === "font" ? "M2 3.5H14V6H9.25V20.5H6.75V6H2Z M17 7.5H19.5V11H22V13.5H19.5V18H22V20.5H17V13.5H14.5V11H17Z"
                    : icon.name === "overview" ? "M3 4H10V10H3Z M14 4H21V10H14Z M3 14H10V20H3Z M14 14H21V20H14Z"
                    : icon.name === "visual" ? "M2 5H22V19H2Z M6.5 10.5H17.5 M9 14.5H15"
                    : icon.name === "markdown" ? "M9.5 3.5L7.5 20.5 M16.5 3.5L14.5 20.5 M4.5 9H20.5 M3.5 15H19.5"
                    : icon.name === "code" ? "M8 5L2 12L8 19 M16 5L22 12L16 19 M14 3L10 21"
                    : icon.name === "file" ? "M6 3H14L19 8V21H6Z M14 3V8H19"
                    : icon.name === "check" ? "M5 12.5L10 17.5L19 7"
                    : icon.name === "history" ? "M3 12A9 9 0 1 0 12 3A9.75 9.75 0 0 0 5.26 5.74L3 8 M3 3V8H8 M12 7V12L16 14"
                    : icon.name === "present" ? "M6 3L21 12L6 21Z"
                    : icon.name === "export" ? "M12 3V15 M7 10L12 15L17 10 M4 15V21H20V15"
                    : icon.name === "bold" ? "M6 4H12A4 4 0 0 1 12 12H6Z M6 12H13A4 4 0 0 1 13 20H6Z"
                    : icon.name === "italic" ? "M10 4H19 M5 20H14 M15 4L9 20"
                    : icon.name === "underline" ? "M7 4V11A5 5 0 0 0 17 11V4 M5 20H19"
                    : icon.name === "headline" ? "M5 4V20 M19 4V20 M5 12H19"
                    : icon.name === "comment" ? "M21 3H3V17H8V21L13 17H21Z M7 8H17 M7 12H14"
                    : icon.name === "media-add" ? "M13 3H3V21H21V11 M3 17L8 12L12 16L15 13L21 19 M19 2V8 M16 5H22 M9 7A1 1 0 1 0 9 9A1 1 0 1 0 9 7"
                    : icon.name === "chevron-down" ? "M6 9L12 15L18 9"
                    : icon.name === "chevron-right" ? "M9 6L15 12L9 18"
                    : icon.name === "adjust" ? "M3 6H7 M11 6H21 M7 3V9H11V3Z M3 18H13 M17 18H21 M13 15V21H17V15Z"
                    : icon.name === "save" ? "M2.5 2.5H10.5L13.5 5.5V13.5H2.5Z M5.5 2.5V6H10V2.5 M4.5 13.5V9.5H11.5V13.5"
                    : "M2.5 13V3.5H6.5L8.5 5.5H13.5V13Z"
            }
        }
    }
}
