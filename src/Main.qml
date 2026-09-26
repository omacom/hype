import QtQuick
import QtQuick.Effects
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import Hype 1.0
import "Markdown.js" as Markdown

ApplicationWindow {
    id: win
    width: 1400; height: 900; minimumWidth: 900; minimumHeight: 600
    visible: true
    title: deck.title + (deck.dirty ? " •" : "") + " — Hype"
    AppTheme { id: appTheme }
    readonly property var ui: appTheme.colors
    readonly property int rounding: appTheme.rounding
    // Small controls soften only when the desktop theme rounds its windows.
    readonly property int softRadius: Math.min(3, rounding)
    color: presenting ? ui.background : ui.panel
    palette.window: win.ui.panel; palette.base: win.ui.background; palette.text: win.ui.foreground
    palette.placeholderText: win.ui.muted
    palette.windowText: win.ui.foreground; palette.button: win.ui.button; palette.buttonText: win.ui.foreground
    palette.highlight: win.ui.selection; palette.highlightedText: win.ui.selectionText
    palette.mid: win.ui.hover; palette.light: win.ui.border; palette.dark: win.ui.button
    property bool markdown: false
    property bool overview: false
    readonly property string mode: overview ? "overview" : markdown ? "markdown" : "visual"
    readonly property bool canFormat: !popupOpen && !deck.compressingImage && !presenting && !overview
    readonly property int rowStep: overview && !presenting ? overviewGrid.columns : 1
    readonly property int inset: 20
    property string flash: ""
    property bool flashFailed: false
    property string seenExportStatus: ""
    function flashStatus(text, failed) {
        flash = text; flashFailed = failed
        flashTimer.interval = failed ? 10000 : 4000
        if (text) flashTimer.restart()
    }
    Timer { id: flashTimer; onTriggered: win.flash = "" }
    Connections {
        target: deck
        function onStatusChanged() { if (!deck.exporting) win.flashStatus(deck.status, false) }
        function onExportChanged() {
            if (deck.exporting || deck.exportStatus === win.seenExportStatus) return
            win.seenExportStatus = deck.exportStatus
            win.flashStatus(deck.exportStatus, deck.exportFailed)
        }
    }
    property bool syncingEditor: false
    property bool editingSlide: false
    property bool presenting: false
    readonly property bool popupOpen: pasteDialog.visible || compressionDialog.visible ||
        historyDialog.visible || closeDialog.visible || shortcutsOverlay.visible || themes.popup.visible || fonts.popup.visible ||
        slideMenu.visible || fileMenu.visible || slideBar.menuOpen || sourceBar.menuOpen
    property var compressionReturnFocus: null
    property bool allowClose: false
    property int lastSelected: -1
    property int dragIndex: -1
    property int dropIndex: -1
    property real dragY: 0
    property real dragX: 0
    property int dragScroll: 0
    function togglePresent() {
        presenting = !presenting
        if (presenting) { win.showFullScreen(); stage.forceActiveFocus() }
        else { player.stop(); win.showNormal() }
    }
    function toggleVideo() {
        if (player.playbackState === MediaPlayer.PlayingState) player.pause()
        else {
            if (player.mediaStatus === MediaPlayer.EndOfMedia) player.position = 0
            player.play()
        }
    }
    function scrollEditor(flick, event) {
        let delta = event.angleDelta.y ? event.angleDelta.y / 120 * 180 : event.pixelDelta.y
        if (!delta) { event.accepted = false; return }
        flick.cancelFlick()
        flick.contentY = Math.max(0, Math.min(Math.max(0, flick.contentHeight - flick.height + flick.bottomMargin), flick.contentY - delta))
        event.accepted = true
    }
    function switchEditingFocus() {
        if (slideEditor.activeFocus || sourceEditor.activeFocus) thumbnails.forceActiveFocus()
        else if (markdown) revealSource(false, sourceFlick.contentY)
        else slideEditor.forceActiveFocus()
    }
    function editorKey(editor, flick, event) {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
            event.accepted = true
            switchEditingFocus()
            return
        }
        if (event.matches(StandardKey.Paste) && deck.pasteMedia()) {
            event.accepted = true
            return
        }
        let control = event.modifiers & Qt.ControlModifier
        if (!win.markdown && editor === slideEditor && event.modifiers === Qt.NoModifier &&
            (event.key === Qt.Key_Home || event.key === Qt.Key_End)) {
            event.accepted = true
            deck.select(event.key === Qt.Key_Home ? 0 : deck.count - 1)
            return
        }
        if (control && event.key === Qt.Key_Z) {
            event.accepted = true
            event.modifiers & Qt.ShiftModifier ? deck.redo() : deck.undo()
            return
        }
        let position = editor.cursorPosition
        let scroll = flick.contentY
        let page = event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp
        if (page) {
            let amount = Math.max(40, flick.height - 40) * (event.key === Qt.Key_PageDown ? 1 : -1)
            let rect = editor.positionToRectangle(position)
            position = editor.positionAt(rect.x, Math.max(0, rect.y + rect.height / 2 + amount))
            scroll += amount
        } else if (event.key === Qt.Key_Home) {
            position = control || position === 0 ? 0 : editor.text.lastIndexOf("\n", position - 1) + 1
        } else if (event.key === Qt.Key_End) {
            let end = editor.text.indexOf("\n", position)
            position = control || end < 0 ? editor.length : end
        } else return
        event.accepted = true
        if (event.modifiers & Qt.ShiftModifier) editor.moveCursorSelection(position, TextEdit.SelectCharacters)
        else editor.cursorPosition = position
        if (page) flick.contentY = Math.max(0, Math.min(Math.max(0, flick.contentHeight - flick.height + flick.bottomMargin), scroll))
    }
    function formatSlide(kind) {
        if (markdown) {
            const sourceEdit = Markdown.format(sourceEditor.text, sourceEditor.selectionStart, sourceEditor.selectionEnd, kind)
            sourceEditor.forceActiveFocus()
            deck.editSource(sourceEdit.text)
            syncEditors()
            sourceEditor.select(sourceEdit.start, sourceEdit.end)
            return
        }
        const edit = Markdown.format(slideEditor.text, slideEditor.selectionStart, slideEditor.selectionEnd, kind)
        slideEditor.forceActiveFocus()
        deck.editSlide(edit.text)
        slideEditor.select(edit.start, edit.end)
    }
    // An existing presentation opens for browsing; a new one opens ready to type.
    function focusForDeck(existing) {
        if (presenting) return
        if (overview) overviewGrid.forceActiveFocus()
        else if (existing) thumbnails.forceActiveFocus()
        else if (markdown) { revealSource(false, sourceFlick.contentY); selectHeadline(sourceEditor, deck.sourcePosition()) }
        else { slideEditor.forceActiveFocus(); selectHeadline(slideEditor, 0) }
    }
    // Selects the placeholder headline's words, leaving its "# ", so typing replaces them.
    function selectHeadline(editor, from) {
        const match = /^#{1,6} +(.+)$/m.exec(editor.text.slice(from))
        if (match) editor.select(from + match.index + match[0].length - match[1].length, from + match.index + match[0].length)
        else editor.cursorPosition = editor.length
    }
    function focusMarkdown() {
        if (overview) setMode("visual")
        if (markdown) revealSource(false, sourceFlick.contentY)
        else slideEditor.forceActiveFocus()
    }
    function openMarkdown() { setMarkdownMode(true) }
    function setMarkdownMode(value) { setMode(value ? "markdown" : "visual") }
    function setMode(name) {
        overview = name === "overview"
        if (!overview) editingMode = name
        markdown = name === "markdown"
        if (overview) {
            overviewGrid.forceActiveFocus()
            Qt.callLater(overviewGrid.revealSelection)
        } else if (markdown) alignSource()
        else stage.forceActiveFocus()
    }
    // Ctrl+M flips between the overview and whichever editing mode you came from.
    property string editingMode: "visual"
    function toggleOverview() { setMode(overview ? editingMode : "overview") }
    function toggleSource() { setMode(markdown && !overview ? "visual" : "markdown") }
    function cycleMode(step) {
        const modes = ["overview", "visual", "markdown"]
        setMode(modes[(modes.indexOf(mode) + step + modes.length) % modes.length])
    }
    // Moves the selected slides by whole rows in the overview, one slide elsewhere.
    function moveSlides(delta) {
        if (Math.abs(delta) === 1) deck.moveSelection(delta)
        else if (delta < 0 && deck.selectionFirst > 0) deck.dropSelection(Math.max(0, deck.selectionFirst + delta))
        else if (delta > 0 && deck.selectionLast < deck.count - 1) deck.dropSelection(Math.min(deck.count, deck.selectionLast + 1 + delta))
        if (markdown) alignSource(false)
    }
    function addSlide() {
        let previousY = sourceFlick.contentY
        deck.addSlide()
        if (overview) return
        if (markdown) revealSource(false, previousY)
        else slideEditor.forceActiveFocus()
    }
    function alignSource(focus = true) { revealSource(true, sourceFlick.contentY, focus) }
    function revealSource(atTop, previousY, focus = true) {
        syncingEditor = true
        sourceEditor.cursorPosition = deck.sourcePosition()
        syncingEditor = false
        if (focus) sourceEditor.forceActiveFocus()
        Qt.callLater(function() {
            let rect = sourceEditor.positionToRectangle(deck.sourcePosition())
            let viewportHeight = win.contentItem.height - 2 * win.inset - sourceBar.height
            let nextY = previousY
            if (atTop || rect.y < previousY + sourceEditor.topPadding)
                nextY = rect.y - sourceEditor.topPadding
            else if (rect.y + rect.height > previousY + viewportHeight - sourceEditor.bottomPadding)
                nextY = rect.y + rect.height + sourceEditor.bottomPadding - viewportHeight
            // Query the document directly: Flickable's contentHeight can still be
            // from the hidden editor until the next layout pass.
            let end = sourceEditor.positionToRectangle(sourceEditor.length)
            let maxY = Math.max(0, end.y + end.height + sourceEditor.bottomPadding - viewportHeight)
            sourceFlick.contentY = Math.max(0, Math.min(maxY, nextY))
        })
    }
    function syncEditors() {
        syncingEditor = true
        if (sourceEditor.text !== deck.source) sourceEditor.text = deck.source
        if (!editingSlide && slideEditor.text !== deck.slideText) slideEditor.text = deck.slideText
        syncingEditor = false
        if (lastSelected !== deck.selected) {
            slideEditor.cursorPosition = 0
            Qt.callLater(function() { slideScroll.contentItem.contentY = 0 })
        }
    }
    Component.onCompleted: { syncEditors(); lastSelected = deck.selected; Qt.callLater(thumbnails.revealSelection); if (deck.path !== "") focusForDeck(true) }
    Connections {
        target: deck
        function onCompressingImageChanged() {
            if (deck.compressingImage) win.compressionReturnFocus = win.activeFocusItem
            else compressionDialog.close()
        }
        function onPasteRequested(name, extension, video) {
            pasteDialog.returnFocus = win.compressionReturnFocus || win.activeFocusItem
            win.compressionReturnFocus = null
            pasteDialog.extension = extension
            pasteDialog.isVideo = video
            pasteDialog.error = ""
            pasteName.text = name
            pasteDialog.open()
        }
        function onChanged() {
            win.syncEditors()
            if (win.dragIndex < 0 && win.lastSelected !== deck.selected) Qt.callLater(thumbnails.revealSelection)
            const nextVideo = deck.media.video ? deck.media.url.toString() : ""
            const changedVideo = win.lastSelected !== deck.selected || player.source.toString() !== nextVideo
            win.lastSelected = deck.selected
            if (changedVideo) {
                player.stop()
                video.clearOutput()
                player.source = nextVideo
                if (win.presenting && deck.media.video && deck.media.autoplay) player.play()
            }
        }
        function onModelAboutToBeReset() {
            thumbnails.cancelFlick()
            thumbnails.stopWheel()
            thumbnails.scrollBeforeReset = thumbnails.contentY - thumbnails.originY
        }
        function onModelReset() { Qt.callLater(thumbnails.revealSelection) }
        function onOpened(existing) { Qt.callLater(function() { win.focusForDeck(existing) }) }
    }
    onPresentingChanged: { if (presenting && deck.media.video && deck.media.autoplay) player.play() }
    onClosing: function(close) {
        if (!allowClose && !deck.flushAutosave() && deck.dirty) {
            close.accepted = false
            closeDialog.open()
        }
    }
    Dialog {
        id: closeDialog; title: "Unsaved changes"; modal: true; anchors.centerIn: parent
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label { text: "Changes could not be backed up. Discard them and quit?" }
        onDiscarded: { win.allowClose = true; win.close() }
    }
    Dialog {
        id: historyDialog; objectName: "historyDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        modal: true; focus: true; title: "Version history"
        width: Math.min(460, win.width - 48); height: Math.min(480, win.height - 80)
        standardButtons: Dialog.Cancel
        property var versions: []
        onOpened: { deck.flushAutosave(); versions = deck.recoveryVersions() }
        contentItem: ColumnLayout {
            Label { text: "Choose a version to restore. Your current version stays in history."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ListView {
                id: versionList
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                model: historyDialog.versions
                ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    text: modelData.label
                    onClicked: if (deck.restoreVersion(modelData.name)) historyDialog.close()
                }
                Label { parent: versionList; anchors.centerIn: parent; visible: versionList.count === 0; text: "No earlier versions yet"; color: win.ui.muted }
            }
        }
    }
    Timer {
        interval: 1000; running: deck.compressingImage
        onTriggered: if (deck.compressingImage) compressionDialog.open()
    }
    Popup {
        id: compressionDialog; objectName: "compressionDialog"
        parent: Overlay.overlay
        popupType: Popup.Item
        modal: true; focus: true; padding: 24
        closePolicy: Popup.CloseOnEscape
        width: Math.min(360, parent.width - 48)
        x: Math.max(16, Math.min(parent.width - width - 16, pasteDialog.center.x - width / 2))
        y: Math.max(16, Math.min(parent.height - height - 16, pasteDialog.center.y - height / 2))
        onClosed: if (deck.compressingImage) deck.cancelPaste()
        background: Rectangle { color: win.ui.panel; border.color: win.ui.border; radius: win.rounding }
        Overlay.modal: Rectangle { color: "#660b0d14" }
        contentItem: ColumnLayout {
            spacing: 18
            Label { text: "Compressing image…"; color: win.ui.foreground; font.pixelSize: 20; font.bold: true }
            ProgressBar {
                Layout.fillWidth: true
                indeterminate: true
                palette.highlight: win.ui.accent
                palette.dark: win.ui.border
                Accessible.name: "Compressing image"
            }
            Button {
                Layout.alignment: Qt.AlignRight
                text: "Cancel"; onClicked: compressionDialog.close()
            }
        }
    }
    Popup {
        id: pasteDialog; objectName: "pasteDialog"
        parent: Overlay.overlay
        popupType: Popup.Item
        modal: true; focus: true; padding: 24
        closePolicy: Popup.CloseOnEscape
        width: Math.min(420, parent.width - 48)
        property string extension: "png"
        property bool isVideo: false
        property string error: ""
        property var returnFocus: null
        property Item target: stage.visible ? slideFrame : sourceScroll
        property point center: {
            // mapToItem does not subscribe to ancestor geometry changes.
            let revision = win.width + win.height + workspace.x + workspace.y +
                stage.x + stage.y + slideFrame.x + slideFrame.y + sourceScroll.x + sourceScroll.y
            return target.mapToItem(parent, target.width / 2, target.height / 2)
        }
        x: Math.max(16, Math.min(parent.width - width - 16, center.x - width / 2))
        y: Math.max(16, Math.min(parent.height - height - 16, center.y - height / 2))
        function save() {
            error = deck.savePastedMedia(pasteName.text)
            if (!error) close()
            else pasteName.forceActiveFocus()
        }
        onOpened: { pasteName.forceActiveFocus(); pasteName.selectAll() }
        onClosed: { deck.cancelPaste(); if (returnFocus) returnFocus.forceActiveFocus() }
        background: Rectangle { color: win.ui.panel; border.color: win.ui.border; radius: win.rounding }
        Overlay.modal: Rectangle { color: "#660b0d14" }
        contentItem: ColumnLayout {
            spacing: 18
            ColumnLayout {
                spacing: 6
                Label { text: pasteDialog.isVideo ? "Paste video" : "Paste image"; color: win.ui.foreground; font.pixelSize: 20; font.bold: true }
                Label { text: "Save to " + (pasteDialog.isVideo ? "videos/" : "images/"); color: win.ui.muted; font.pixelSize: 14 }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                TextField {
                    id: pasteName; objectName: "pasteName"
                    Accessible.name: "Filename"
                    Layout.fillWidth: true; implicitHeight: 44
                    font.pixelSize: 16; color: win.ui.foreground
                    selectionColor: win.ui.selection; selectedTextColor: win.ui.selectionText
                    leftPadding: 12; rightPadding: 12
                    background: Rectangle { color: win.ui.background; radius: Math.min(4, win.rounding); border.color: pasteName.activeFocus ? win.ui.accent : win.ui.border }
                    onTextEdited: pasteDialog.error = ""
                    onAccepted: if (text.trim()) pasteDialog.save()
                }
                Label { text: "." + pasteDialog.extension; color: win.ui.muted; font.pixelSize: 16 }
            }
            Label {
                Layout.fillWidth: true; visible: text.length > 0
                text: pasteDialog.error; color: win.ui.error; font.pixelSize: 13; wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Item { Layout.fillWidth: true }
                Button {
                    id: cancelPasteButton; text: "Cancel"; implicitHeight: 38; implicitWidth: 84
                    contentItem: Text { text: parent.text; color: win.ui.foreground; font.pixelSize: 14; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: cancelPasteButton.hovered ? win.ui.hover : win.ui.button; radius: Math.min(4, win.rounding); border.color: cancelPasteButton.activeFocus ? win.ui.accent : "transparent" }
                    onClicked: pasteDialog.close()
                }
                Button {
                    id: savePasteButton; text: "Save " + (pasteDialog.isVideo ? "video" : "image")
                    implicitHeight: 38; implicitWidth: 112; enabled: pasteName.text.trim().length > 0
                    contentItem: Text { text: parent.text; color: win.ui.accentText; font.pixelSize: 14; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: !savePasteButton.enabled ? win.ui.border : savePasteButton.hovered ? win.ui.accentHover : win.ui.accent; radius: Math.min(4, win.rounding); border.color: savePasteButton.activeFocus ? win.ui.foreground : "transparent" }
                    onClicked: pasteDialog.save()
                }
            }
        }
    }
    Shortcut { sequences: ["Tab", "Shift+Tab"]; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && (stage.activeFocus || thumbnails.activeFocus); onActivated: win.switchEditingFocus() }
    Shortcut { enabled: !win.popupOpen && !deck.compressingImage; sequences: [StandardKey.Open]; onActivated: deck.openDialog() }
    Shortcut { enabled: !win.popupOpen && !deck.compressingImage; sequences: [StandardKey.Save]; onActivated: deck.save() }
    Shortcut { enabled: !win.popupOpen && !deck.compressingImage; sequences: [StandardKey.SaveAs]; onActivated: deck.saveAs() }
    Shortcut { sequence: "Ctrl+E"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !deck.exporting; onActivated: deck.exportDialog("pdf") }
    Shortcut { sequence: "Ctrl+Shift+E"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !deck.exporting; onActivated: deck.exportDialog("pptx") }
    Shortcut { sequences: ["?", "Shift+?"]; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: shortcutsOverlay.open() }
    Shortcut { sequence: "F1"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting; onActivated: shortcutsOverlay.open() }
    Shortcut { sequence: "Ctrl+M"; enabled: !win.popupOpen && !deck.compressingImage && (!win.presenting); onActivated: win.toggleOverview() }
    Shortcut { sequence: "Ctrl+."; enabled: !win.popupOpen && !deck.compressingImage && (!win.presenting); onActivated: win.toggleSource() }
    Shortcut { sequence: "Ctrl+B"; enabled: win.canFormat; onActivated: win.formatSlide("bold") }
    Shortcut { sequence: "Ctrl+I"; enabled: win.canFormat; onActivated: win.formatSlide("italic") }
    Shortcut { sequence: "Ctrl+U"; enabled: win.canFormat; onActivated: win.formatSlide("underline") }
    Shortcut { sequence: "Ctrl+H"; enabled: win.canFormat; onActivated: win.formatSlide("headline") }
    Shortcut { sequence: "Ctrl+K"; enabled: win.canFormat; onActivated: win.formatSlide("code") }
    Shortcut { sequence: "Ctrl+/"; enabled: win.canFormat; onActivated: win.formatSlide("comment") }
    Shortcut { sequences: ["Return", "Enter"]; enabled: !win.popupOpen && !deck.compressingImage && win.overview && !win.presenting; onActivated: win.focusMarkdown() }
    Shortcut { enabled: !win.popupOpen && !deck.compressingImage; sequence: "Ctrl+N"; onActivated: deck.newDeck() }
    Shortcut { enabled: !win.popupOpen && !deck.compressingImage; sequences: ["F5", "Ctrl+Space"]; autoRepeat: false; onActivated: win.togglePresent() }
    Shortcut { sequence: "Escape"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting); onActivated: win.togglePresent() }
    Shortcut { sequence: "Ctrl+Z"; enabled: !win.popupOpen && !deck.compressingImage && (!slideEditor.activeFocus && !sourceEditor.activeFocus); onActivated: deck.undo() }
    Shortcut { sequence: "Ctrl+Shift+Z"; enabled: !win.popupOpen && !deck.compressingImage && (!slideEditor.activeFocus && !sourceEditor.activeFocus); onActivated: deck.redo() }
    Shortcut { sequence: "Ctrl+D"; enabled: !win.popupOpen && !deck.compressingImage && (!slideEditor.activeFocus && !sourceEditor.activeFocus); onActivated: deck.duplicateSlide() }
    Shortcut { enabled: !win.popupOpen && !deck.compressingImage; sequence: "Ctrl+Return"; onActivated: { win.addSlide() } }
    Shortcut { sequence: "Delete"; enabled: !win.popupOpen && !deck.compressingImage && (!slideEditor.activeFocus && !sourceEditor.activeFocus); onActivated: deck.deleteSlide() }
    Shortcut { sequence: "Right"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!slideEditor.activeFocus && !sourceEditor.activeFocus)); onActivated: deck.select(deck.selected + 1) }
    Shortcut { sequence: "Ctrl+Right"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: win.moveSlides(1) }
    Shortcut { sequence: "Shift+Right"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: { deck.extendSelection(deck.selected + 1); if (win.markdown) win.alignSource(false) } }
    Shortcut { sequence: "Down"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!slideEditor.activeFocus && !sourceEditor.activeFocus)); onActivated: deck.select(deck.selected + (win.presenting ? 1 : win.rowStep)) }
    Shortcut { sequence: "Ctrl+Down"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: win.moveSlides(win.rowStep) }
    Shortcut { sequence: "Shift+Down"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: { deck.extendSelection(deck.selected + win.rowStep); if (win.markdown) win.alignSource(false) } }
    Shortcut { sequence: "Left"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!slideEditor.activeFocus && !sourceEditor.activeFocus)); onActivated: deck.select(deck.selected + -1) }
    Shortcut { sequence: "Ctrl+Left"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: win.moveSlides(-1) }
    Shortcut { sequence: "Shift+Left"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: { deck.extendSelection(deck.selected + -1); if (win.markdown) win.alignSource(false) } }
    Shortcut { sequence: "Up"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!slideEditor.activeFocus && !sourceEditor.activeFocus)); onActivated: deck.select(deck.selected + (win.presenting ? -1 : -win.rowStep)) }
    Shortcut { sequence: "Ctrl+Up"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: win.moveSlides(-win.rowStep) }
    Shortcut { sequence: "Shift+Up"; enabled: !win.popupOpen && !deck.compressingImage && !win.presenting && !slideEditor.activeFocus && !sourceEditor.activeFocus; onActivated: { deck.extendSelection(deck.selected + -win.rowStep); if (win.markdown) win.alignSource(false) } }
    Shortcut { sequence: "PgDown"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!slideEditor.activeFocus && !sourceEditor.activeFocus)); onActivated: deck.select(deck.selected + 5 * win.rowStep) }
    Shortcut { sequence: "PgUp"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!slideEditor.activeFocus && !sourceEditor.activeFocus)); onActivated: deck.select(deck.selected - 5 * win.rowStep) }
    Shortcut { sequence: "Home"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!win.markdown && !slideEditor.activeFocus)); onActivated: deck.select(0) }
    Shortcut { sequence: "End"; enabled: !win.popupOpen && !deck.compressingImage && (win.presenting || (!win.markdown && !slideEditor.activeFocus)); onActivated: deck.select(deck.count - 1) }
    Shortcut { sequence: "Space"; enabled: !win.popupOpen && !deck.compressingImage && win.presenting && (deck.media.video || animation.active); autoRepeat: false; onActivated: { if (animation.item) animation.item.paused = !animation.item.paused; else win.toggleVideo() } }
    Shortcut { sequence: "Ctrl+V"; enabled: !win.popupOpen && !deck.compressingImage && (!slideEditor.activeFocus && !sourceEditor.activeFocus); onActivated: deck.pasteMedia() }
    component ToolbarIconButton: ToolButton {
        id: toolbarButton
        required property string iconName
        required property string description
        property bool primary: false
        property color ink: win.ui.muted
        property var dropdown: null
        property bool dropdownWasOpen: false
        onPressed: dropdownWasOpen = !!dropdown && dropdown.wasOpenOnPress()
        onClicked: if (dropdown) dropdown.toggle(dropdownWasOpen)
        readonly property bool lit: hovered || down
        Layout.preferredWidth: primary ? 40 : 36; Layout.preferredHeight: 36
        Layout.leftMargin: primary ? 9 : 0
        padding: 0
        Accessible.name: description
        ToolTip.visible: hovered; ToolTip.text: description
        contentItem: Item {
            AppIcon {
                anchors.centerIn: parent; width: 18; height: 18; name: toolbarButton.iconName; opacity: toolbarButton.enabled ? 1 : 0.4
                color: toolbarButton.primary ? win.ui.accentText : toolbarButton.lit ? win.ui.foreground : toolbarButton.ink
            }
        }
        background: Rectangle {
            radius: win.softRadius
            color: toolbarButton.primary ? (toolbarButton.lit ? win.ui.accentHover : win.ui.accent) : toolbarButton.lit ? win.ui.hover : "transparent"
        }
    }
    component EditorButton: ToolButton {
        id: editorButton
        required property string iconName
        required property string label
        required property string description
        property bool menu: false
        property bool compact: false
        property var dropdown: null
        property bool dropdownWasOpen: false
        onPressed: dropdownWasOpen = !!dropdown && dropdown.wasOpenOnPress()
        onClicked: if (dropdown) dropdown.toggle(dropdownWasOpen)
        readonly property color ink: hovered || down ? win.ui.foreground : win.ui.muted
        Layout.preferredHeight: 32
        leftPadding: 10; rightPadding: 10; topPadding: 0; bottomPadding: 0
        focusPolicy: Qt.NoFocus
        Accessible.name: description
        ToolTip.visible: hovered; ToolTip.text: description
        contentItem: Row {
            spacing: 7; opacity: editorButton.enabled ? 1 : 0.4
            AppIcon { visible: !editorButton.menu || editorButton.compact; anchors.verticalCenter: parent.verticalCenter; width: 16; height: 16; name: editorButton.iconName; color: editorButton.ink }
            Label { visible: !editorButton.compact; anchors.verticalCenter: parent.verticalCenter; text: editorButton.label; font.pixelSize: 13; color: editorButton.ink }
            AppIcon { visible: editorButton.menu; anchors.verticalCenter: parent.verticalCenter; width: 11; height: 11; name: "chevron-down"; color: editorButton.ink }
        }
        background: Rectangle { color: editorButton.hovered || editorButton.down ? win.ui.hover : "transparent"; radius: win.softRadius }
    }
    // A slide thumbnail in its frame, with the border drawn straight over the picture's edge.
    // The picture stays square; on rounded themes a ring in the surrounding panel colour
    // covers its corners, so they follow the frame without an offscreen mask.
    component SlideFrame: Rectangle {
        id: frame
        required property int slide
        required property bool selected
        required property bool hovered
        property size renderSize: Qt.size(340, 192)
        readonly property bool current: deck.selected === slide
        color: deck.background; radius: win.rounding
        Image {
            anchors.fill: parent
            source: "image://slides/" + (deck.revision, deck.renderId(frame.slide))
            asynchronous: true; retainWhileLoading: true; cache: true
            sourceSize: frame.renderSize; fillMode: Image.PreserveAspectCrop; clip: true
        }
        // A ring's inner radius is its radius minus its width, so this one sits outside
        // the frame and lands exactly on the frame's own corners.
        Rectangle {
            visible: frame.radius > 0; anchors.fill: parent; anchors.margins: -border.width; color: "transparent"
            radius: frame.radius + border.width; border.width: 4; border.color: win.ui.panel
        }
        Rectangle {
            anchors.fill: parent; radius: frame.radius; color: "transparent"
            border.width: frame.current ? 3 : frame.selected ? 2 : 1
            border.color: frame.selected ? win.ui.accent : win.ui.border
        }
        SlideBadge { slide: frame.slide; current: frame.current; hovered: frame.hovered }
    }
    // Names a slide from inside its frame: part of the accent highlight on the
    // current slide, and a quieter tab on whichever slide the pointer is over.
    component SlideBadge: Rectangle {
        required property int slide
        required property bool current
        required property bool hovered
        visible: current || (hovered && win.dragIndex < 0)
        anchors.left: parent.left; anchors.bottom: parent.bottom
        width: badgeLabel.implicitWidth + 12; height: badgeLabel.implicitHeight + 6
        color: current ? win.ui.accent : win.ui.border
        topRightRadius: win.softRadius; bottomLeftRadius: win.rounding
        Label { id: badgeLabel; anchors.centerIn: parent; text: "Slide " + (parent.slide + 1); font.pixelSize: 10; color: parent.current ? win.ui.accentText : win.ui.foreground }
    }
    component AppMenu: Menu {
        id: appMenu
        property bool hasChecks: false
        property real closedAt: 0
        onClosed: closedAt = Date.now()
        // Pressing the button that owns an open menu first dismisses it as an outside press;
        // without this the click that follows would open it again.
        function wasOpenOnPress() { return visible || Date.now() - closedAt < 150 }
        function toggle(wasOpen) { if (wasOpen) close(); else open() }
        padding: 6; margins: 8
        // Wide enough for the longest label plus its shortcut hint.
        implicitWidth: {
            let widest = 160
            for (let i = 0; i < count; ++i) widest = Math.max(widest, itemAt(i).implicitWidth)
            return widest + leftPadding + rightPadding
        }
        background: Rectangle { color: win.ui.panel; border.color: win.ui.border; radius: win.rounding }
        delegate: AppMenuItem {}
    }
    component AppMenuItem: MenuItem {
        id: appMenuItem
        property string hint
        property bool destructive: false
        readonly property color ink: destructive ? win.ui.error : win.ui.foreground
        implicitHeight: 32
        implicitWidth: leftPadding + itemLabel.implicitWidth + (hint ? 28 + itemHint.implicitWidth : 0) + rightPadding
        leftPadding: menu && menu.hasChecks ? 32 : 12; rightPadding: 12; topPadding: 0; bottomPadding: 0
        font.pixelSize: 13
        indicator: AppIcon {
            x: 10; anchors.verticalCenter: parent.verticalCenter; width: 14; height: 14
            visible: appMenuItem.checkable && appMenuItem.checked; name: "check"; color: win.ui.accent
        }
        arrow: null
        contentItem: Item {
            opacity: appMenuItem.enabled ? 1 : 0.4
            Label { id: itemLabel; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: appMenuItem.text; font: appMenuItem.font; color: appMenuItem.ink }
            Label { id: itemHint; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: appMenuItem.hint; font.pixelSize: 12; color: win.ui.muted }
        }
        background: Rectangle { radius: Math.min(4, win.rounding); color: appMenuItem.highlighted && appMenuItem.enabled ? win.ui.hover : "transparent" }
    }
    component AppMenuSeparator: MenuSeparator {
        topPadding: 5; bottomPadding: 5; leftPadding: 6; rightPadding: 6
        contentItem: Rectangle { implicitHeight: 1; color: win.ui.border; opacity: 0.7 }
        background: null
    }
    component EditorToolbar: ToolBar {
        id: editorBar
        property string scope: ""
        property real textInset: 24
        readonly property bool compact: width < 680
        readonly property bool menuOpen: mediaMenu.visible
        Layout.preferredHeight: 48
        background: Rectangle {
            color: win.ui.background
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.ui.border; opacity: 0.55 }
        }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: editorBar.textInset - 10; anchors.rightMargin: 14; spacing: 2
            EditorButton { compact: editorBar.compact; objectName: editorBar.scope + "boldButton"; iconName: "bold"; label: "Bold"; description: "Bold (Ctrl+B)"; onClicked: win.formatSlide("bold") }
            EditorButton { compact: editorBar.compact; objectName: editorBar.scope + "italicButton"; iconName: "italic"; label: "Italic"; description: "Italic (Ctrl+I)"; onClicked: win.formatSlide("italic") }
            EditorButton { compact: editorBar.compact; objectName: editorBar.scope + "underlineButton"; iconName: "underline"; label: "Underline"; description: "Underline (Ctrl+U)"; onClicked: win.formatSlide("underline") }
            EditorButton { compact: editorBar.compact; objectName: editorBar.scope + "headlineButton"; iconName: "headline"; label: "Headline"; description: "Headline (Ctrl+H)"; onClicked: win.formatSlide("headline") }
            EditorButton { compact: editorBar.compact; objectName: editorBar.scope + "codeButton"; iconName: "code"; label: "Code"; description: "Code block (Ctrl+K)"; onClicked: win.formatSlide("code") }
            EditorButton { compact: editorBar.compact; objectName: editorBar.scope + "commentButton"; iconName: "comment"; label: "Note"; description: "Comment, hidden on slide (Ctrl+/)"; onClicked: win.formatSlide("comment") }
            Item { Layout.fillWidth: true }
            EditorButton { compact: editorBar.compact; iconName: "media-add"; label: "Media"; description: "Add image / video"; onClicked: deck.importDialog() }
            EditorButton {
                compact: editorBar.compact
                objectName: editorBar.scope + "mediaOptionsButton"; iconName: "adjust"; label: "Layout"; description: "Image / video options"; menu: true
                enabled: !!deck.media.url.toString()
                dropdown: mediaMenu
                AppMenu {
                    hasChecks: true
                    id: mediaMenu; objectName: editorBar.scope + "mediaMenu"; y: parent.height + 4
                    AppMenuItem { text: "Fit"; checkable: true; checked: !deck.media.span; onTriggered: deck.setMediaMode("fit") }
                    AppMenuItem { text: "Span"; checkable: true; checked: deck.media.span; onTriggered: deck.setMediaMode("span") }
                    AppMenuSeparator {}
                    AppMenuItem { text: "Match image edges"; checkable: true; checked: deck.media.background === "auto"; onTriggered: deck.matchImageBackground(true) }
                    AppMenuItem { text: deck.media.video ? "Blurred first frame" : "Blurred image"; checkable: true; checked: deck.media.background === "blur"; onTriggered: deck.setMediaBackground("blur") }
                    AppMenuItem { text: "White"; checkable: true; checked: deck.media.background === "white"; onTriggered: deck.setMediaBackground("white") }
                    AppMenuItem { text: "Black"; checkable: true; checked: deck.media.background === "black"; onTriggered: deck.setMediaBackground("black") }
                    AppMenuItem { text: "Use theme color"; checkable: true; checked: deck.media.background === "theme"; onTriggered: deck.matchImageBackground(false) }
                }
            }
        }
    }
    header: ToolBar {
        id: topBar
        visible: !win.presenting; height: visible ? 60 : 0
        background: Rectangle {
            color: win.ui.panel
            Rectangle { x: win.inset; width: parent.width - 2 * win.inset; height: 1; anchors.bottom: parent.bottom; color: win.ui.border }
        }
        Label {
            id: logo; objectName: "hypeLogo"
            anchors.left: parent.left; anchors.leftMargin: win.inset; anchors.verticalCenter: parent.verticalCenter
            text: "Hype"; font.pixelSize: 22; font.bold: true
            color: logoHover.hovered ? win.ui.accentHover : win.ui.accent
            Accessible.role: Accessible.Button; Accessible.name: "Keyboard shortcuts"
            ToolTip.visible: logoHover.hovered; ToolTip.delay: 400; ToolTip.text: "Keyboard shortcuts (?)"
            HoverHandler { id: logoHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: shortcutsOverlay.open() }
        }
        Column {
            id: summary; objectName: "deckSummary"
            readonly property real room: Math.max(0, topBar.width - 2 * (Math.max(toolbarActions.width, logo.width) + win.inset + 20))
            anchors.centerIn: parent; spacing: 2
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: deck.title; color: win.ui.foreground
                width: Math.min(implicitWidth, summary.room); elide: Text.ElideRight
                ToolTip.visible: summaryHover.hovered && truncated; ToolTip.text: text
                HoverHandler { id: summaryHover }
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                objectName: "deckStatus"
                width: Math.min(implicitWidth, summary.room); elide: Text.ElideRight
                text: deck.exporting ? deck.exportStatus
                    : win.flash ? win.flash
                    : deck.selectionCount > 1 ? deck.selectionCount + " slides selected" : "Slide " + (deck.selected+1) + " of " + deck.count
                font.pixelSize: 12; color: win.flash && win.flashFailed && !deck.exporting ? win.ui.error : win.ui.muted
                ToolTip.visible: statusHover.hovered && truncated; ToolTip.text: text
                HoverHandler { id: statusHover }
            }
        }
        Rectangle {
            visible: deck.exporting; Accessible.name: "Export progress"
            x: win.inset; anchors.bottom: parent.bottom
            width: (parent.width - 2 * win.inset) * deck.exportProgress; height: 2; color: win.ui.accent
        }
        RowLayout {
            id: toolbarActions; objectName: "toolbarActions"
            anchors.right: parent.right; anchors.rightMargin: win.inset; anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            ToolButton {
                id: cancelExport
                visible: deck.exporting; Layout.preferredHeight: 36; leftPadding: 10; rightPadding: 10
                contentItem: Label { text: "Cancel export"; font.pixelSize: 12; color: cancelExport.hovered ? win.ui.foreground : win.ui.muted; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: cancelExport.hovered || cancelExport.down ? win.ui.hover : "transparent"; radius: win.softRadius }
                onClicked: deck.cancelExport()
            }
            ComboBox {
                id: themes; objectName: "themePicker"
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                padding: 0; indicator: null
                contentItem: Item {
                    AppIcon { anchors.centerIn: parent; width: 18; height: 18; name: "theme"; color: themes.hovered || themes.popup.visible ? win.ui.foreground : win.ui.muted }
                }
                Accessible.name: "Theme: " + currentText
                ToolTip.visible: hovered; ToolTip.text: "Theme: " + currentText
                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: themes.popup.availableWidth
                    text: modelData
                    highlighted: themes.highlightedIndex === index
                    contentItem: Text { text: modelData; color: win.ui.foreground; font: themes.font; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: parent.highlighted ? win.ui.hover : win.ui.panel }
                }
                background: Rectangle { color: themes.hovered || themes.popup.visible ? win.ui.hover : "transparent"; radius: win.softRadius }
                model: deck.themeNames
                currentIndex: Math.max(0, deck.themeNames.indexOf(deck.themeName))
                onActivated: deck.chooseTheme(currentText)
                popup: Popup {
                    y: themes.height + 4; width: 240; padding: 6
                    height: Math.min(contentItem.implicitHeight + 12, 420, win.height - 100)
                    background: Rectangle { color: win.ui.panel; border.color: win.ui.border; radius: win.rounding }
                    contentItem: ListView {
                        clip: true; implicitHeight: contentHeight
                        model: themes.popup.visible ? themes.delegateModel : null
                        currentIndex: themes.highlightedIndex
                        ScrollBar.vertical: ScrollBar {}
                    }
                    onOpened: contentItem.positionViewAtIndex(themes.currentIndex, ListView.Contain)
                }
            }
            ComboBox {
                id: fonts; objectName: "fontPicker"
                property bool showNotoVariants: false
                readonly property var visibleFonts: deck.fontNames.filter(function(name) {
                    return showNotoVariants || !name.startsWith("Noto ") ||
                        ["Noto Sans", "Noto Serif", "Noto Sans Mono", deck.fontName].indexOf(name) >= 0
                })
                model: visibleFonts.concat([showNotoVariants ? "Fewer Noto fonts" : "More Noto fonts…"])
                currentIndex: visibleFonts.indexOf(deck.fontName)
                displayText: deck.fontName
                onActivated: function(index) {
                    if (index === visibleFonts.length) {
                        showNotoVariants = !showNotoVariants
                        currentIndex = Qt.binding(function() { return fonts.visibleFonts.indexOf(deck.fontName) })
                        Qt.callLater(function() { fonts.popup.open() })
                    } else deck.chooseFont(visibleFonts[index])
                }
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                padding: 0; indicator: null
                contentItem: Item {
                    AppIcon { anchors.centerIn: parent; width: 18; height: 18; name: "font"; color: fonts.hovered || fonts.popup.visible ? win.ui.foreground : win.ui.muted }
                }
                Accessible.name: "Font: " + deck.fontName
                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: fonts.popup.availableWidth; text: modelData
                    highlighted: fonts.highlightedIndex === index
                    contentItem: Text { text: modelData; color: win.ui.foreground; font: fonts.font; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: parent.highlighted ? win.ui.hover : win.ui.panel }
                }
                background: Rectangle { color: fonts.hovered || fonts.popup.visible ? win.ui.hover : "transparent"; radius: win.softRadius }
                popup: Popup {
                    y: fonts.height + 4; width: 320; padding: 6
                    height: Math.min(contentItem.implicitHeight + 12, 420, win.height - 100)
                    background: Rectangle { color: win.ui.panel; border.color: win.ui.border; radius: win.rounding }
                    contentItem: ListView {
                        clip: true; implicitHeight: contentHeight
                        model: fonts.popup.visible ? fonts.delegateModel : null
                        currentIndex: fonts.highlightedIndex
                        ScrollBar.vertical: ScrollBar {}
                    }
                    onOpened: contentItem.positionViewAtIndex(fonts.currentIndex, ListView.Contain)
                }
                ToolTip.visible: hovered; ToolTip.text: "Font: " + deck.fontName
            }
            ToolbarIconButton {
                objectName: "modeButton"
                iconName: win.mode
                description: (win.overview ? "Overview · Switch to Visual" : win.markdown ? "Markdown · Switch to Overview" : "Visual · Switch to Markdown") + "  ·  Ctrl+M overview, Ctrl+. source"
                onClicked: win.cycleMode(1)
            }
            ToolbarIconButton {
                objectName: "fileButton"; iconName: "file"; ink: deck.dirty ? win.ui.accent : win.ui.muted
                description: deck.dirty ? "File · Unsaved changes" : "File"
                dropdown: fileMenu
                AppMenu {
                    id: fileMenu; objectName: "fileMenu"
                    y: parent.height + 4
                    AppMenuItem { text: "New presentation"; hint: "Ctrl+N"; onTriggered: deck.newDeck() }
                    AppMenuItem { text: "Open…"; hint: "Ctrl+O"; onTriggered: deck.openDialog() }
                    AppMenuSeparator {}
                    AppMenuItem { text: deck.dirty ? "Save changes" : "Save"; hint: "Ctrl+S"; onTriggered: deck.save() }
                    AppMenuItem { text: "Save as…"; hint: "Ctrl+Shift+S"; onTriggered: deck.saveAs() }
                    AppMenuSeparator {}
                    AppMenuItem { text: "Export as PDF…"; hint: "Ctrl+E"; enabled: !deck.exporting; onTriggered: deck.exportDialog("pdf") }
                    AppMenuItem { text: "Export as PowerPoint…"; hint: "Ctrl+Shift+E"; enabled: !deck.exporting; onTriggered: deck.exportDialog("pptx") }
                    AppMenuSeparator {}
                    AppMenuItem { text: "Version history…"; onTriggered: historyDialog.open() }
                }
            }
            ToolbarIconButton {
                objectName: "presentButton"; iconName: "present"; description: "Present (Ctrl+Space)"; primary: true
                onClicked: win.togglePresent()
            }
        }
    }
    Popup {
        id: shortcutsOverlay; objectName: "shortcutsOverlay"
        parent: Overlay.overlay; anchors.centerIn: parent
        modal: true; focus: true; padding: 28
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.35) }
        background: Rectangle { color: Qt.alpha(win.ui.panel, 0.92); border.color: win.ui.windowBorder; border.width: 2; radius: win.rounding }
        readonly property var groups: [
            { title: "Presentation", keys: [
                ["Ctrl+N", "New presentation"], ["Ctrl+O", "Open"], ["Ctrl+S", "Save"], ["Ctrl+Shift+S", "Save as"],
                ["Ctrl+E", "Export as PDF"], ["Ctrl+Shift+E", "Export as PowerPoint"], ["Ctrl+Space / F5", "Present"], ["Esc", "Stop presenting"],
                ["Space", "Play or pause video while presenting"] ] },
            { title: "View", keys: [
                ["Ctrl+M", "Overview on or off"], ["Ctrl+.", "Markdown source on or off"],
                ["Tab", "Switch between slides and editor"], ["Enter", "Open slide from Overview"],
                ["? / F1", "Show these shortcuts"] ] },
            { title: "Slides", keys: [
                ["Arrows", "Previous or next slide, by row in Overview"], ["Page Up / Page Down", "Jump five slides, or five rows in Overview"],
                ["Home / End", "First or last slide"], ["Shift+Arrows", "Extend the selection"],
                ["Ctrl+Arrows", "Move selected slides"], ["Ctrl+Enter", "Add a slide"],
                ["Ctrl+D", "Duplicate"], ["Delete", "Delete"] ] },
            { title: "Editing", keys: [
                ["Ctrl+B", "Bold"], ["Ctrl+I", "Italic"], ["Ctrl+U", "Underline"], ["Ctrl+H", "Headline"], ["Ctrl+K", "Code block"],
                ["Ctrl+/", "Comment, hidden on slide"],
                ["Ctrl+Z", "Undo"], ["Ctrl+Shift+Z", "Redo"], ["Ctrl+V", "Paste text, or add and name media"] ] }
        ]
        contentItem: ColumnLayout {
            spacing: 22; focus: true
            // Window shortcuts are blocked while a modal popup is open, so toggle from here.
            Keys.onPressed: function(event) {
                if (event.text === "?" || event.key === Qt.Key_F1) { shortcutsOverlay.close(); event.accepted = true }
            }
            Label { text: "Keyboard shortcuts"; font.pixelSize: 16; font.bold: true; color: win.ui.foreground }
            GridLayout {
                columns: win.width < 820 ? 1 : 2; columnSpacing: 48; rowSpacing: 22
                Repeater {
                    model: shortcutsOverlay.groups
                    ColumnLayout {
                        required property var modelData
                        Layout.alignment: Qt.AlignTop; spacing: 7
                        Label { text: modelData.title.toUpperCase(); font.pixelSize: 11; font.letterSpacing: 1; color: win.ui.accent; Layout.bottomMargin: 2 }
                        Repeater {
                            model: modelData.keys
                            RowLayout {
                                required property var modelData
                                spacing: 14
                                Label { text: modelData[0].replace(/\+/g, " + "); font.family: "JetBrains Mono"; font.pixelSize: 12; color: win.ui.foreground; Layout.preferredWidth: 180 }
                                Label { text: modelData[1]; font.pixelSize: 13; color: win.ui.muted }
                            }
                        }
                    }
                }
            }
        }
    }
    // Shared by the sidebar and the overview, so it cannot live inside either.
    AppMenu { id: slideMenu
        AppMenuItem { text: "New slide after this"; hint: "Ctrl+Enter"; onTriggered: { win.addSlide() } }
        AppMenuItem { text: deck.selectionCount > 1 ? "Duplicate slides" : "Duplicate slide"; hint: "Ctrl+D"; onTriggered: deck.duplicateSlide() }
        AppMenuSeparator {}
        AppMenuItem { text: deck.selectionCount > 1 ? "Delete slides" : "Delete slide"; hint: "Del"; destructive: true; onTriggered: deck.deleteSlide() }
    }
    RowLayout {
        anchors.fill: parent; spacing: 0
        GridView {
            id: overviewGrid; objectName: "overviewGrid"
            visible: win.overview && !win.presenting
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            // Cells carry their gap on the right, so the grid runs to the window edge.
            Layout.margins: win.inset; Layout.rightMargin: 0
            readonly property real gap: win.inset
            readonly property int columns: Math.max(1, Math.floor(width / (260 + gap)))
            readonly property real tileWidth: Math.floor(width / columns) - gap
            readonly property real tileHeight: Math.round(tileWidth * 9 / 16)
            property int hoveredSlide: -1
            property int dropColumn: 0
            property int dropRow: 0
            cellWidth: tileWidth + gap; cellHeight: tileHeight + gap
            // No tiles are built or rendered until the overview is actually showing.
            model: win.overview ? deck : null; currentIndex: deck.selected
            cacheBuffer: height
            highlightFollowsCurrentItem: false
            keyNavigationEnabled: false
            boundsBehavior: Flickable.StopAtBounds
            // Rows always rest against the top padding: a half-scrolled row would show
            // its trailing gap there and make the padding look deeper than in other modes.
            snapMode: GridView.SnapToRow
            bottomMargin: contentHeight > height ? height % cellHeight : 0
            function revealSelection() {
                forceLayout()
                positionViewAtIndex(deck.selected, GridView.Contain)
                contentY = originY + Math.ceil((contentY - originY) / cellHeight - 0.001) * cellHeight
            }
            onColumnsChanged: if (win.overview) Qt.callLater(revealSelection)
            onHeightChanged: if (win.overview) Qt.callLater(revealSelection)
            function updateDrop(x, y) {
                win.dragX = x
                win.dragY = y
                win.dragScroll = y < 36 ? -1 : (y > height - 36 ? 1 : 0)
                // Drop between slides, using each thumbnail's midpoint.
                const rows = Math.ceil(deck.count / columns)
                dropRow = Math.max(0, Math.min(rows - 1, Math.floor((y + contentY - originY) / cellHeight)))
                const filled = Math.min(columns, deck.count - dropRow * columns)
                dropColumn = Math.max(0, Math.min(filled, Math.floor((x + cellWidth / 2) / cellWidth)))
                win.dropIndex = dropRow * columns + dropColumn
            }
            function cancelDrag() {
                win.dragIndex = -1
                win.dropIndex = -1
                win.dragScroll = 0
            }
            Connections {
                target: deck
                function onSelectedChanged() { if (win.overview && win.dragIndex < 0) Qt.callLater(overviewGrid.revealSelection) }
            }
            delegate: Item {
                id: tile; required property int index
                width: overviewGrid.cellWidth; height: overviewGrid.cellHeight
                property bool selected: index >= deck.selectionFirst && index <= deck.selectionLast
                opacity: win.dragIndex >= 0 && selected ? 0.4 : 1
                HoverHandler { id: tileHover; onHoveredChanged: overviewGrid.hoveredSlide = hovered ? tile.index : overviewGrid.hoveredSlide === tile.index ? -1 : overviewGrid.hoveredSlide }
                SlideFrame {
                    width: overviewGrid.tileWidth; height: overviewGrid.tileHeight
                    slide: tile.index; selected: tile.selected; hovered: tileHover.hovered; renderSize: Qt.size(480, 270)
                }
            }
            Rectangle {
                parent: overviewGrid; z: 2
                visible: win.overview && win.dragIndex >= 0 && win.dropIndex >= 0
                width: 3; height: overviewGrid.tileHeight; color: win.ui.accent
                x: Math.max(0, overviewGrid.dropColumn * overviewGrid.cellWidth - overviewGrid.gap / 2 - 1)
                y: overviewGrid.originY + overviewGrid.dropRow * overviewGrid.cellHeight - overviewGrid.contentY
            }
            MouseArea {
                id: tileDrag; parent: overviewGrid; anchors.fill: parent; z: 1
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                preventStealing: true
                cursorShape: win.dragIndex >= 0 ? Qt.ClosedHandCursor : Qt.ArrowCursor
                property point pressPosition
                property int pressedIndex: -1
                property bool moved: false
                property bool extending: false
                function tileAt(x, y) {
                    // The gap beside and below a thumbnail belongs to no slide.
                    const column = Math.floor(x / overviewGrid.cellWidth)
                    const row = Math.floor((y + overviewGrid.contentY - overviewGrid.originY) / overviewGrid.cellHeight)
                    if (x - column * overviewGrid.cellWidth > overviewGrid.tileWidth) return -1
                    if (y + overviewGrid.contentY - overviewGrid.originY - row * overviewGrid.cellHeight > overviewGrid.tileHeight) return -1
                    return overviewGrid.indexAt(x, y + overviewGrid.contentY)
                }
                onPressed: function(mouse) {
                    overviewGrid.cancelFlick()
                    pressPosition = Qt.point(mouse.x, mouse.y)
                    pressedIndex = tileAt(mouse.x, mouse.y)
                    moved = false
                    overviewGrid.forceActiveFocus()
                    if (pressedIndex < 0) return
                    extending = !!(mouse.modifiers & Qt.ShiftModifier)
                    if (extending) deck.extendSelection(pressedIndex)
                    else if (pressedIndex < deck.selectionFirst || pressedIndex > deck.selectionLast) deck.select(pressedIndex)
                    if (mouse.button === Qt.RightButton) slideMenu.popup()
                }
                onPositionChanged: function(mouse) {
                    if (!(pressedButtons & Qt.LeftButton) || pressedIndex < 0) return
                    if (win.dragIndex < 0 && Math.hypot(mouse.x - pressPosition.x, mouse.y - pressPosition.y) > 8) {
                        win.dragIndex = pressedIndex
                        moved = true
                    }
                    if (win.dragIndex >= 0) overviewGrid.updateDrop(mouse.x, mouse.y)
                }
                onReleased: function(mouse) {
                    if (win.dragIndex >= 0) {
                        overviewGrid.updateDrop(mouse.x, mouse.y)
                        let slot = win.dropIndex
                        overviewGrid.cancelDrag()
                        if (slot >= 0) deck.dropSelection(slot)
                        overviewGrid.revealSelection()
                    }
                    if (!moved && !extending && mouse.button === Qt.LeftButton && pressedIndex >= 0) deck.select(pressedIndex)
                    pressedIndex = -1
                }
                onCanceled: { overviewGrid.cancelDrag(); pressedIndex = -1 }
                onDoubleClicked: if (!moved && tileAt(mouseX, mouseY) >= 0) win.focusMarkdown()
                onWheel: function(wheel) { wheel.accepted = false }
            }
            Timer {
                interval: 40; repeat: true; running: win.overview && win.dragIndex >= 0 && win.dragScroll !== 0
                onTriggered: {
                    overviewGrid.contentY = Math.max(overviewGrid.originY, Math.min(
                        overviewGrid.originY + Math.max(0, overviewGrid.contentHeight - overviewGrid.height),
                        overviewGrid.contentY + win.dragScroll * 18))
                    overviewGrid.updateDrop(win.dragX, win.dragY)
                }
            }
        }
        Rectangle {
            visible: !win.presenting && !win.overview; Layout.preferredWidth: 227; Layout.fillHeight: true; color: win.ui.panel
            ColumnLayout {
                anchors.fill: parent; anchors.margins: win.inset; spacing: 10
                ListView {
                    id: thumbnails; objectName: "thumbnails"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: deck; spacing: 10; currentIndex: deck.selected
                    cacheBuffer: height * 2
                    highlightFollowsCurrentItem: false
                    keyNavigationEnabled: false // Window shortcuts drive the selection.
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                    property int hoveredSlide: -1
                    property int wheelDirection: 0
                    property real wheelRemainder: 0
                    readonly property real thumbnailHeight: 108
                    readonly property real slideStep: thumbnailHeight + spacing
                    property real scrollBeforeReset: -1
                    function revealSelection() {
                        forceLayout()
                        if (scrollBeforeReset >= 0) {
                            const maximum = Math.max(0, count * slideStep - spacing - height)
                            contentY = originY + Math.min(scrollBeforeReset, maximum)
                            scrollBeforeReset = -1
                        }
                        positionViewAtIndex(deck.selected, ListView.Contain)
                    }
                    function stopWheel() {
                        wheelDirection = 0
                        wheelRemainder = 0
                    }
                    function scrollWheel(event) {
                        if (slideDrag.pressed) { event.accepted = true; return }
                        let pixels = event.pixelDelta.y
                        let delta = event.angleDelta.y || pixels
                        if (!delta) { event.accepted = false; return }
                        cancelFlick()
                        let direction = Math.sign(delta)
                        if (direction !== wheelDirection) wheelRemainder = 0
                        wheelDirection = direction
                        // Accumulate high-resolution input, but always move whole slides.
                        // Prefer wheel angles even when Qt also supplies a pixel delta.
                        wheelRemainder += event.angleDelta.y ? event.angleDelta.y / 120 : pixels / slideStep
                        let steps = Math.trunc(wheelRemainder)
                        if (!steps) { event.accepted = true; return }
                        wheelRemainder -= steps
                        thumbnails.forceActiveFocus()
                        deck.select(deck.selected - steps)
                        if (win.markdown) win.alignSource(false)
                        event.accepted = true
                    }
                    WheelHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        target: null
                        onWheel: function(event) { thumbnails.scrollWheel(event) }
                    }
                    function updateDrop(x, y) {
                        win.dragX = x
                        win.dragY = y
                        if (x < 0 || x > width) {
                            win.dropIndex = -1
                            win.dragScroll = 0
                            return
                        }
                        win.dragScroll = y < 36 ? -1 : (y > height - 36 ? 1 : 0)
                        // Drop between slides, using each thumbnail's midpoint.
                        win.dropIndex = Math.max(0, Math.min(deck.count,
                            Math.floor((y + contentY - originY + slideStep / 2) / slideStep)))
                    }
                    function cancelDrag() {
                        win.dragIndex = -1
                        win.dropIndex = -1
                        win.dragScroll = 0
                    }
                    onMovementStarted: stopWheel()
                    delegate: Item {
                        id: thumbnail; required property int index
                        HoverHandler { id: thumbnailHover; onHoveredChanged: thumbnails.hoveredSlide = hovered ? thumbnail.index : thumbnails.hoveredSlide === thumbnail.index ? -1 : thumbnails.hoveredSlide }
                        width: thumbnails.width; height: thumbnails.thumbnailHeight
                        property bool selected: index >= deck.selectionFirst && index <= deck.selectionLast
                        opacity: win.dragIndex >= 0 && selected ? 0.4 : 1
                        SlideFrame {
                            width: parent.width; height: parent.height
                            slide: thumbnail.index; selected: thumbnail.selected; hovered: thumbnailHover.hovered
                        }
                    }
                    Rectangle {
                        parent: thumbnails; z: 2
                        visible: !win.overview && win.dragIndex >= 0 && win.dropIndex >= 0
                        width: thumbnails.width; height: 3
                        y: Math.max(0, Math.min(thumbnails.height - height,
                            thumbnails.originY + win.dropIndex * thumbnails.slideStep - thumbnails.contentY - thumbnails.spacing / 2))
                        color: win.ui.accent
                    }
                    MouseArea {
                        id: slideDrag; parent: thumbnails; anchors.fill: parent; z: 1
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        preventStealing: true
                        cursorShape: win.dragIndex >= 0 ? Qt.ClosedHandCursor : Qt.ArrowCursor
                        property point pressPosition
                        property int pressedIndex: -1
                        property bool moved: false
                        property bool extending: false
                        onPressed: function(mouse) {
                            thumbnails.cancelFlick()
                            thumbnails.stopWheel()
                            pressPosition = Qt.point(mouse.x, mouse.y)
                            pressedIndex = thumbnails.indexAt(mouse.x, mouse.y + thumbnails.contentY)
                            moved = false
                            if (pressedIndex < 0) return
                            extending = !!(mouse.modifiers & Qt.ShiftModifier)
                            if (extending) deck.extendSelection(pressedIndex)
                            else if (pressedIndex < deck.selectionFirst || pressedIndex > deck.selectionLast) deck.select(pressedIndex)
                            thumbnails.forceActiveFocus()
                            if (mouse.button === Qt.RightButton) slideMenu.popup()
                        }
                        onPositionChanged: function(mouse) {
                            if (!(pressedButtons & Qt.LeftButton) || pressedIndex < 0) return
                            if (win.dragIndex < 0 && Math.hypot(mouse.x - pressPosition.x, mouse.y - pressPosition.y) > 8) {
                                win.dragIndex = pressedIndex
                                moved = true
                            }
                            if (win.dragIndex >= 0) thumbnails.updateDrop(mouse.x, mouse.y)
                        }
                        onReleased: function(mouse) {
                            if (win.dragIndex >= 0) {
                                thumbnails.updateDrop(mouse.x, mouse.y)
                                let slot = win.dropIndex
                                thumbnails.cancelDrag()
                                if (slot >= 0) deck.dropSelection(slot)
                                thumbnails.positionViewAtIndex(deck.selected, ListView.Contain)
                            }
                            if (!moved && !extending && mouse.button === Qt.LeftButton && pressedIndex >= 0) deck.select(pressedIndex)
                            if (win.markdown && pressedIndex >= 0) win.alignSource(false)
                            pressedIndex = -1
                        }
                        onCanceled: { thumbnails.cancelDrag(); pressedIndex = -1 }
                        onDoubleClicked: if (!moved) win.focusMarkdown()
                    }
                    Timer {
                        interval: 40; repeat: true; running: !win.overview && win.dragIndex >= 0 && win.dragScroll !== 0
                        onTriggered: {
                            thumbnails.contentY = Math.max(thumbnails.originY, Math.min(
                                thumbnails.originY + thumbnails.contentHeight - thumbnails.height,
                                thumbnails.contentY + win.dragScroll * 18))
                            thumbnails.updateDrop(win.dragX, win.dragY)
                        }
                    }
                }
            }
        }
        SplitView {
            id: workspace; orientation: Qt.Vertical
            visible: (!win.markdown && !win.overview) || win.presenting
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.margins: win.presenting ? 0 : win.inset; Layout.leftMargin: 0
            background: Rectangle { color: win.ui.background }
            handle: Rectangle {
                implicitHeight: win.presenting ? 0 : 6
                color: SplitHandle.hovered || SplitHandle.pressed ? win.ui.accent : win.ui.border
            }
            Item {
                id: stage; objectName: "stage"; SplitView.fillHeight: true; SplitView.minimumHeight: 160; focus: true
                property real margin: win.presenting ? 0 : 32
                property real slideWidth: Math.min(width-margin*2,(height-margin*2)*16/9)
                Item {
                    id: slideFrame; objectName: "slideFrame"; width: stage.slideWidth; height: width*9/16; anchors.centerIn: parent
                    // Keep the fast thumbnail behind the full render. Even if it remains visible
                    // briefly after loading, it cannot cover the sharp image once that is ready.
                    Image {
                        objectName: "slideQuickPreview"; anchors.fill: parent
                        visible: slidePreview.status === Image.Loading && slidePreview.shownSlide !== deck.selected && status === Image.Ready
                        // The same URL and size as the sidebar thumbnail, so it comes from Qt's pixmap cache.
                        source: "image://slides/" + (deck.revision, deck.renderId(deck.selected))
                        asynchronous: true; cache: true; sourceSize: Qt.size(340, 192)
                    }
                    Image {
                        id: slidePreview; objectName: "slidePreview"; anchors.fill: parent
                        source: "image://slides/" + (deck.revision, deck.renderId(deck.selected)) + (animation.active ? "/background" : "")
                        asynchronous: true; retainWhileLoading: true; cache: true; sourceSize: Qt.size(1920, 1080)
                        property int shownSlide: -1
                        // On a new slide, let its thumbnail show while this render loads. Edits to
                        // the same slide retain the previous sharp frame until the new one is ready.
                        visible: status === Image.Ready || shownSlide === deck.selected
                        onStatusChanged: if (status === Image.Ready) shownSlide = deck.selected
                    }
                    Loader {
                        id: animation; objectName: "animationLoader"
                        active: workspace.visible && !!deck.media.animated
                        x: deck.media.rect.x * slideFrame.width / 1920
                        y: deck.media.rect.y * slideFrame.height / 1080
                        width: deck.media.rect.width * slideFrame.width / 1920
                        height: deck.media.rect.height * slideFrame.height / 1080
                        sourceComponent: AnimatedImage {
                            objectName: "animatedMedia"
                            source: deck.media.url
                            asynchronous: true
                            cache: false // Decode the current frame without retaining an entire animation.
                            playing: true
                            property bool autoplay: deck.media.autoplay
                            paused: !autoplay
                            onAutoplayChanged: paused = !autoplay
                            fillMode: deck.media.span ? Image.PreserveAspectCrop : Image.PreserveAspectFit
                            clip: true
                            layer.enabled: !!deck.media.title
                            layer.effect: MultiEffect {
                                blurEnabled: true
                                blurMax: 4
                                blur: Math.min(1, 0.5 * slideFrame.width / 1920)
                                autoPaddingEnabled: false
                            }
                            onSourceChanged: paused = !autoplay
                            onStatusChanged: if (status === Image.Ready) paused = !autoplay
                        }
                    }
                    VideoOutput {
                        id: video; objectName: "videoOutput"
                        visible: deck.media.video && (player.playbackState !== MediaPlayer.StoppedState || player.mediaStatus === MediaPlayer.EndOfMedia)
                        endOfStreamPolicy: VideoOutput.KeepLastFrame
                        x: deck.media.rect.x * slideFrame.width / 1920
                        y: deck.media.rect.y * slideFrame.height / 1080
                        width: deck.media.rect.width * slideFrame.width / 1920
                        height: deck.media.rect.height * slideFrame.height / 1080
                        fillMode: deck.media.span ? VideoOutput.PreserveAspectCrop : VideoOutput.PreserveAspectFit
                    }
                    Image { anchors.fill: parent; source: visible ? "image://slides/" + (deck.revision, deck.renderId(deck.selected)) + "/overlay" : ""; asynchronous: true; retainWhileLoading: true; sourceSize: Qt.size(1920,1080); visible: animation.active || (video.visible && deck.media.span) }
                    Button { visible: deck.media.video && !win.presenting; anchors.centerIn: parent; text: player.playbackState === MediaPlayer.PlayingState ? "Pause" : "▶ Play"; onClicked: win.toggleVideo() }
                }
                DropArea { anchors.fill: parent; onDropped: function(drop) { if (drop.hasUrls) for (let i = 0; i < drop.urls.length; ++i) { if (!deck.importMedia(drop.urls[i], i > 0)) break } } }
            }
            ColumnLayout {
                id: editorPane; objectName: "editorPane"
                visible: !win.presenting; spacing: 0
                SplitView.preferredHeight: 250; SplitView.minimumHeight: 140
                SplitView.maximumHeight: workspace.height * 0.65
            EditorToolbar { id: slideBar; Layout.fillWidth: true }
            ScrollView {
                id: slideScroll; objectName: "slideScroll"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                WheelHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    target: null
                    onWheel: function(event) { win.scrollEditor(slideScroll.contentItem, event) }
                }
                TextArea {
                    id: slideEditor; objectName: "slideEditor"; persistentSelection: true; textFormat: TextEdit.PlainText; color: win.ui.foreground; selectionColor: win.ui.selection; selectedTextColor: win.ui.selectionText; font.family: "JetBrains Mono"; font.pixelSize: 16
                    wrapMode: TextEdit.Wrap; leftPadding: 24; topPadding: 16; placeholderText: "# Your headline"
                    onTextChanged: {
                        if (!win.syncingEditor && activeFocus) {
                            const previousCount = deck.count
                            win.editingSlide = true
                            deck.editSlide(text)
                            win.editingSlide = false
                            if (deck.count !== previousCount) win.syncEditors()
                        }
                    }
                    Keys.onPressed: function(event) { win.editorKey(slideEditor, slideScroll.contentItem, event) }
                }
            }
            }
        }
        ColumnLayout {
            visible: win.markdown && !win.overview && !win.presenting; spacing: 0
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.margins: win.inset; Layout.leftMargin: 0
        EditorToolbar { id: sourceBar; scope: "source."; textInset: sourceEditor.leftPadding; Layout.fillWidth: true }
        ScrollView {
            id: sourceScroll; objectName: "sourceScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            background: Rectangle { color: win.ui.background }
            Flickable {
                id: sourceFlick; objectName: "sourceFlick"
                clip: true; boundsBehavior: Flickable.StopAtBounds
                WheelHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    target: null
                    onWheel: function(event) { win.scrollEditor(sourceFlick, event) }
                }
                TextArea.flickable: TextArea {
                id: sourceEditor; objectName: "sourceEditor"
                persistentSelection: true
                textFormat: TextEdit.PlainText
                color: win.ui.foreground; selectionColor: win.ui.selection; selectedTextColor: win.ui.selectionText
                font.family: "JetBrains Mono"; font.pixelSize: 18
                wrapMode: TextEdit.NoWrap; leftPadding: 32; topPadding: 30; bottomPadding: 30
                onTextChanged: { if (!win.syncingEditor && activeFocus && text !== deck.source) deck.editSource(text) }
                onCursorPositionChanged: { if (!win.syncingEditor && activeFocus) deck.selectAt(cursorPosition) }
                Keys.onPressed: function(event) { win.editorKey(sourceEditor, sourceFlick, event) }
            }
            }
        }
        }
    }
    MediaPlayer { id: player; objectName: "player"; videoOutput: video; audioOutput: AudioOutput { muted: deck.media.muted } loops: deck.media.loop ? MediaPlayer.Infinite : 1; onErrorOccurred: function(error,message) { deck.setStatus(message) } }
}
