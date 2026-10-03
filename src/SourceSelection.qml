import QtQuick

// A viewport-sized decoration, independent of native text selection and hit testing.
// Only two document positions are queried, even when thousands of slides are selected.
Item {
    id: mark
    required property var presentation
    required property var editor
    required property var flickable
    required property bool selectionFocused
    required property color inactiveStroke
    required property color accent
    property real rounding: 0
    readonly property int firstSlide: presentation.selectionFirst
    readonly property int lastSlide: presentation.selectionLast
    readonly property int revision: presentation.revision
    property real documentTop: 0
    property real documentBottom: 0
    readonly property real rangeTop: documentTop - flickable.contentY
    readonly property real rangeBottom: documentBottom - flickable.contentY
    // A border can be onscreen while its text is still clipped by TextArea's
    // padding. Use the readable area when deciding whether a jump is needed.
    readonly property bool above: rangeTop + 7 < editor.topPadding
    readonly property bool below: rangeBottom - 7 > height - editor.bottomPadding
    readonly property bool startOutside: above || rangeTop >= height
    readonly property bool endOutside: below || rangeBottom <= 0
    clip: true

    function refresh() {
        if (!visible) return
        const start = Math.min(editor.length, presentation.sourceSelectionStart)
        // Before a separator, its preceding newline belongs to the selected slide.
        // At EOF, include the final empty line after a trailing newline as well.
        const lastPosition = presentation.sourceSelectionEnd - (lastSlide < presentation.count - 1 ? 1 : 0)
        const end = Math.min(editor.length, Math.max(start, lastPosition))
        const first = editor.positionToRectangle(start)
        const last = editor.positionToRectangle(end)
        documentTop = editor.mapToItem(flickable.contentItem, 0, first.y).y - 7
        documentBottom = editor.mapToItem(flickable.contentItem, 0, last.y + last.height).y + 7
    }
    function revealBoundary(end) {
        refresh()
        flickable.cancelFlick()
        // TextArea's attached Flickable clips text within its padding. Keep the
        // boundary line clear of that padding instead of aligning it at y=0.
        const target = end ? documentBottom - height + editor.bottomPadding
                           : documentTop - editor.topPadding
        const maximum = Math.max(0, flickable.contentHeight - height + flickable.bottomMargin)
        flickable.contentY = Math.max(0, Math.min(maximum, target))
    }
    Component.onCompleted: Qt.callLater(refresh)
    onVisibleChanged: if (visible) Qt.callLater(refresh)
    onFirstSlideChanged: Qt.callLater(refresh)
    onLastSlideChanged: Qt.callLater(refresh)
    onRevisionChanged: Qt.callLater(refresh)
    Connections {
        target: mark.editor
        function onTextChanged() { Qt.callLater(mark.refresh) }
        function onContentHeightChanged() { Qt.callLater(mark.refresh) }
        function onWidthChanged() { Qt.callLater(mark.refresh) }
        function onFontChanged() { Qt.callLater(mark.refresh) }
    }
    Rectangle {
        objectName: "sourceSelectionOutline"
        x: 12; width: Math.max(0, mark.width - 24)
        // Keep the actual end caps offscreen for a continued selection, rather
        // than drawing a misleading closed box around just its visible part.
        y: Math.max(-8, mark.rangeTop)
        height: Math.max(0, Math.min(mark.height + 8, mark.rangeBottom) - y)
        visible: mark.selectionFocused && mark.rangeBottom > 0 && mark.rangeTop < mark.height
        color: "transparent"
        radius: Math.min(3, mark.rounding)
        border.width: 2
        border.color: mark.presentation.selectionCount > 1 ? mark.accent : mark.inactiveStroke
    }
}
