# Hype

A small, local presentation app. Write Markdown, arrange slides visually, present fullscreen, and export PDF or PowerPoint. A presentation is a directory you can copy, sync, or put in Git.

This is the product plan. A first working build now exists; see [README.md](../README.md) for supported behavior and [TRIALS.md](../TRIALS.md) for the Rails World trials. PDF and PowerPoint are rendered output formats; Markdown remains the editable source.

Visual concepts: [visual editor](renderings/visual-editor.png) and [Markdown editor](renderings/markdown-editor.png). Treat these as layout and visual direction, not exact widget or syntax specifications: the implementation will use Linux window decoration and valid Markdown fences. The mockups do not yet show the optional per-slide text editor.

![Visual editor concept](renderings/visual-editor.png)

![Markdown editor concept](renderings/markdown-editor.png)

## What the existing presentations suggest

References are actually under `~/Dropbox/Documents/Presentations/`. Inspected the 2024 and 2025 Rails World PowerPoint files, sampled their rendered PDFs, and inspected the 2026 Omacon PDF and ODP.

- Rails World 2024: 106 slides; 63 have no text elements in the PowerPoint. Large monospace headlines, solid backgrounds, logos, screenshots, and code.
- Rails World 2025: 122 slides; 59 have no text elements. Full-slide pictures and memes, large headlines, screenshots, code, and some comparisons. Caskaydia Mono variants are prominent in the text elements.
- Omacon 2026: 117 PDF pages, with image backgrounds, large headlines, longer quotes, and selective colored emphasis. Its ODP contains 118 slide elements and two media plugin elements; investigate the extra slide when constructing reference fixtures.

Prioritize headline, picture, headline over picture, code, quote, and video slides. Images need both **fit** (preserve screenshots, logos, and portraits) and **fill** (crop backgrounds). Support basic Markdown paragraphs, emphasis, and short lists too. Support simple Markdown tables for text comparisons and metrics. Freeform multi-column composition can wait; existing composite pictures can already be used as images.

## The editing experience

```text
┌──────────────────────────────────────────────────────────────────┐
│ Hype · My talk           Visual / Markdown    Theme   Present Export│
├───────────────┬──────────────────────────────────────────────────┤
│ + New slide   │                                                  │
│               │                                                  │
│ 1 [thumbnail] │                  Selected slide                  │
│ 2 [thumbnail] │                                                  │
│ 3 [selected ] │                                                  │
│ 4 [thumbnail] │                                                  │
│               ├──────────────────────────────────────────────────┤
│               │ Add image / video · Fit / Fill · Background      │
└───────────────┴──────────────────────────────────────────────────┘
```

The left strip is always the slide list. All slide organization works from the visual editor and updates the Markdown document immediately:

- **Drag to reorder:** drag a thumbnail to an insertion marker between slides. On drop, move the entire corresponding Markdown slide block, including its directives and comments. Keep the moved slide selected. The document order is the only slide order; there is no separate visual ordering to synchronize later.
- **New slide:** a visible `+ New slide` button inserts a blank slide after the selection, selects it, and focuses its per-slide Markdown editor. Offer the same action from the thumbnail context menu and `Ctrl+Enter`. An empty deck uses this action to create its first slide.
- **Duplicate:** a thumbnail context-menu action and `Ctrl+D` duplicate the selected slide immediately after itself and select the copy. Copy the source verbatim; reuse referenced media files rather than duplicating assets. These shortcuts apply to the slide list or canvas, leaving text-editor shortcuts alone.
- **Delete and undo:** deleting removes the source block but retains its assets. Each reorder, addition, duplication, or deletion is one undoable document operation. Undo restores the source and selection together.

The Markdown pane shows the result immediately; saving writes that same document to disk. Source edits likewise update the thumbnail list. Empty slides must remain real source blocks, including at the beginning or end of the deck.

The right pane shows the selected slide as a preview. A per-slide Markdown editor is always visible below it, separated by a draggable divider. Keep layout automatic, with a few alignment and media controls. Direct canvas text editing is a later enhancement.

Selecting a thumbnail updates both the preview and the Markdown pane below it. Visual mode keeps both panes visible. A single button shows the current mode (Visual or Markdown); clicking it or pressing Ctrl+E switches to a full-document Markdown editor and back, preserving selection and edits; the selected source starts at the top. Presentation mode hides the editor. Visual operations modify the same source document. A font dropdown beside the theme selects an installed font and stores it in front matter; code remains monospaced.

Use a conventional Save action and a dirty indicator. Detect external changes: reload a clean document; when local edits exist, offer Reload or Save a Copy rather than silently overwriting. Save atomically. Defer automatic crash recovery and merging. One document-owned undo history spans source edits and visual operations; route both editors through it and disable their separate undo histories.

Fullscreen presentation uses arrows to navigate, Space for video playback, and Escape to return to editing. Stop video when leaving its slide. On a laptop with an external display, the audience view opens automatically on the external display while a separate presenter window shows the current slide, next slide, and notes from HTML comments on the built-in display. If the editor is already on an external display, keep using it for the audience view.

## A small Markdown format

```text
my-talk/
  presentation.md
  images/
    city.jpg
    screenshot.png
  videos/
    demo.mp4
```

Allow any `.md` filename with sibling asset directories; `presentation.md` is the default. Use initial YAML front matter for deck settings, `---` on its own line between slides, and optional `hype` comments for slide properties. Images use Markdown embed syntax: `![](city.jpg)`. Optional media directives go inside the brackets: `![span](city.jpg)`. Hype interprets these labels as rendering instructions; ordinary Markdown viewers treat them as alt text. Keep media location and slide layout separate: the file extension selects `images/` or `videos/`, while the surrounding slide content determines placement. Hype supplies the directory for bare filenames; explicit Markdown paths such as `![](images/city.jpg)` work too. Video uses the same embed notation, `![](demo.mp4)`, interpreted as a playable video by Hype. No special image syntax, background declaration, or poster declaration is needed for ordinary slides.

````markdown
---
title: My talk
theme: tokyo-night
font: Caskaydia Mono
---

# The browser is forever

---

![](city.jpg)

# Celebrating computers

---

![](screenshot.png)

---

```ruby
class Post < ApplicationRecord
  belongs_to :author
end
```

---

![](demo.mp4)
````

Defaults do most of the work: a heading becomes a large centered headline; a lone image fits without cropping; an image with a heading becomes a full-bleed background with centered white text and a 25% black overlay, regardless of their order in the source. Whenever text overlays a picture, apply a very light blur to that picture (roughly two pixels at 1080p, scaled for 4K), keeping the text sharp. Apply it consistently in previews, presentation, PDF, and PowerPoint, including animated pictures; never modify the source asset. A lone video fits without cropping, starts once when its slide is entered during presentation, and stops on leaving. Videos stay paused in the editor. Generate a poster automatically from the first frame. A heading with a video sits above it. A lone code block fills the safe content area; an optional heading reserves title space. A heading followed by one short paragraph becomes a headline with a smaller subtitle; longer body text stays in the title/body layout. Quotes receive a readable, left-aligned text layout without a decorative quote bar; a following paragraph starting with an em dash is the smaller attribution. A slide containing only a paragraph with explicit hard breaks becomes a centered stack of large lines, without bullets. Lists retain their bullets or numbering and align left. Simple Markdown tables use evenly spaced columns and quiet rules, with a shared fitted size. Explicit Markdown hard breaks control headline lines.

Only write directives when overriding a default. Put media directives inside `[]`, separated by spaces:

```markdown
![](screenshot.png)
![span](city.jpg)
![fit](diagram.png)
![span loop muted](demo.mp4)
![autoplay=false poster=demo.jpg](demo.mp4)
```

- Empty brackets use the inferred layout and playback defaults.
- `span` fills the slide edge-to-edge, cropping centrally as needed. A heading overlays it. Applies to images and videos.
- `fit` preserves the entire image or video without cropping. Image text remains overlaid; video headings reserve title space. This overrides automatic image spanning.
- `left` or `right` fits an image into the corresponding half of the slide. Image text remains overlaid on the slide.
- `loop` and `muted` enable video looping and mute its audio. Both default off. `autoplay=false` starts playback on Space instead of slide entry.
- `overlay=0.4` overrides the black scrim opacity, from zero to one. Default to 0.25 with overlaid text, zero without it. `poster=demo.jpg` selects a custom video poster from `images/`.

Values containing spaces are quoted, for example `poster="demo still.jpg"`. Reserve the first bracket token to distinguish instructions from ordinary alt text: a recognized directive or a `key=value` token starts a directive list; otherwise the entire label is ordinary alt text. Use `alt="A city at night"` to supply alt text alongside directives, or to escape a label that is itself a directive word. Diagnose unknown directives and conflicting `span fit` flags; preserve the source. Visual controls patch these tokens and remove overrides restored to their defaults.

Keep the optional slide-level `hype` comment only for background color, foreground, and text alignment. Media layout and playback belong on the media reference. Emphasis uses the deck accent; slide foreground/background overrides support accent-colored title slides. Do not add coordinates or arbitrary styling in version one.

Parse slide boundaries outside fenced code and front matter only. Document that a top-level `---` is reserved for a slide break; use `***` for a thematic rule. Preserve source slices, comments, whitespace, and unrecognized metadata when moving slides. Version one supports inline media references with filenames, including Markdown angle-bracket destinations for names containing spaces: `![](<city at night.jpg>)`. Explicit `images/` and `videos/` prefixes are accepted without adding the directory twice, but imports always insert the shorter filename form. Classify known image/video extensions case-insensitively; unknown extensions receive a diagnostic rather than a directory search. Preserve reference-style definitions in source but flag them as unsupported until resolution across slides is implemented. A visual edit patches only the affected source range; do not regenerate the entire file through a Markdown serializer. Unsupported content stays in the source and receives a visible diagnostic.

Freeze these format rules before implementing the editor:

- A line scanner records slide and block ranges in the document's UTF-16 offsets, matching Qt text edits; UTF-8 conversion happens only at file boundaries. Recognize backtick and tilde fences of arbitrary valid length. Front matter occurs only at the beginning, after an optional UTF-8 BOM. Treat top-level `---` as a boundary before rendering Markdown; underline-style headings using `---` are unsupported.
- Moving a slide preserves its body verbatim and normalizes only touched boundaries to a blank line, `---`, and a blank line. Test moving the last slide first, the first last, duplicate-last, and trailing empty slides.
- Front matter initially supports flat scalar YAML fields only, with a real parser for that documented subset. Save palette roles as flat `color_background`, `color_foreground`, `color_accent`, and syntax-role keys; reject unsupported nested settings visibly without deleting them.
- A `hype` directive belongs immediately before the first content block. Use double-quoted string values with escaped quotes/backslashes. Allow one directive per slide; diagnose duplicates. Preserve unknown keys and patch only the changed attribute. An overlay is a black scrim with opacity from zero to one; foreground is separately controllable.
- Determine layout from these combinations. Use Qt's Markdown text support for rendering supported text after Hype has extracted structure; never use it to round-trip the source.

| Slide content | Layout |
| --- | --- |
| Heading only | Centered headline |
| Heading plus one short paragraph | Headline with smaller subtitle |
| Heading plus longer paragraphs or short list | Title above body |
| Paragraph with hard breaks only | Centered stack of large lines, no bullets |
| Unordered or ordered list, optional heading | Large left-aligned points, with bullets or numbers |
| Simple Markdown table, optional heading | Comparison or metrics columns, fitted together |
| One image | Contained image |
| One image plus heading, optional body | Full-bleed image with centered white text and 25% black overlay |
| Image with `fit` or `background=blur`, plus text | Contained image with white text overlaid and 25% darkening |
| Image or video with `span` | Full-bleed media, with any heading overlaid |
| Image with `left` or `right` | Image positioned in a fixed half-slide area, with text overlaid |
| One fenced code block, optional heading | Fitted code with optional title space |
| Blockquote, optional attribution paragraph | Quote with attribution below |
| One video reference, optional heading | Contained video with optional title space; play once on entering during presentation |
| Other combinations | Vertical stack preview with unsupported-layout diagnostic; source remains editable |

## Images and videos

File picker, clipboard image paste, and drag/drop all copy assets into the deck's `images/` or `videos/` directory and insert filename-only media references. Ask for a presentation directory before the first asset import into an unsaved deck. Retain readable names, reuse identical files, and suffix conflicting names without overwriting. Import failures leave the Markdown untouched.

Pasted still images use a 3840 × 2160 pixel budget: fit within it, or retain enough pixels to fill it when spanning. Never upscale or destructively crop the saved asset. Compare lossless PNG and WebP encodings; retain existing files if smaller and already within budget. Keep animations and SVG files intact. PDF retains vector text, losslessly embeds images at their visible 4K size, and strips hidden spanning pixels from the export only. Lossless photographs may be larger than the previous JPEG-compressed PDF output.

Deleting a slide leaves its files in place. Avoid automatic media cleanup in version one. A moved presentation folder must still open and export without the original source files. Broken paths show an actionable placeholder and block export until resolved.

Use Qt Multimedia for playback and ffprobe/ffmpeg for media information and poster generation, following Omacut's process wrappers. Embed compatible H.264/AAC MP4 files directly in PowerPoint; convert other video formats to temporary MP4 copies during export without changing the originals. Report invalid or unreadable videos with a slide number. Generated poster images live in `images/` under a reserved name derived from the video content hash; resolve or regenerate them automatically without adding a poster attribute to the Markdown. Explicitly chosen posters override this convention. Runtime thumbnails live in the application cache.

## Scaling and themes

Lay out once on a logical 1920 × 1080 canvas, then scale that result uniformly for preview, thumbnails, fullscreen, and exports. Version one supports 16:9 only and diagnoses other aspect values. Headlines start at a consistent deck-wide size and shrink only on overflow; respect deliberate line breaks and measure the complete text block within safe margins. Cap code size too, so tiny examples do not become disproportionately large. Use design metrics in logical units, avoiding device-specific hinting differences.

Code fitting measures the widest line and total block height using actual font metrics, then chooses the largest size that fits both dimensions. Preserve indentation, expand tabs consistently, and never wrap or clip code silently. Warn when fitting would make it too small to read. Inline backtick spans inherit the surrounding text scale. Use GNU source-highlight for language-tagged fences, with token colors mapped to the selected theme. Run it off the UI thread during preview rendering and cache results by language and code; text fitting never reruns highlighting. Unlabelled or unsupported languages remain plain. Preserve the measured fonts and layout when applying colors. The runtime package is `source-highlight`.

Discover installed themes using Omarchy's actual theme locations: `$OMARCHY_PATH/themes/` and `~/.config/omarchy/themes/`, with user themes taking precedence. The checked-out Omarchy currently stores the selected theme under `~/.local/state/omarchy/current/theme`; use the installation/XDG paths rather than copying a path assumption from an older app.

Read each theme's `colors.toml`. Map background, foreground, accent, and semantic colors to slides and code highlighting. Theme selection previews the entire deck. Choosing a presentation theme must not change the desktop theme.

For fitted images and videos, `background=blur` stretches a blurred copy across the slide behind the sharp foreground. Offer it in the Background menu alongside edge matching and theme color; choosing blur switches spanning media to fit. Cache the blur and use the same rendering for previews and exports. Videos and animated images use a still first-frame background; a custom video poster does not replace that background. Video edge matching (`background=auto`) likewise samples the first frame and keeps that color during playback; choosing it switches spanning videos to fit.

Finished videos hold their last frame using Qt 6.9's video output retention. Space restarts them from the beginning, while paused videos resume. Clear retained frames when changing media.

The app interface follows the active desktop theme independently, watching its colors file and theme symlink for changes. Palette and font icons open the presentation pickers; the footer uses Omawrite's save/open icons. The title includes slide count and the combined size of the Markdown and unique referenced media. File pickers use the XDG desktop portal, as in Omacut, with “Open File” and “Save File” titles. Omarchy recognizes the portal's windows and opens them centered and floating.

Snapshot the resolved palette into the deck's YAML front matter when choosing a theme. Keep its name as attribution and offer an explicit refresh from the installed theme. This makes colors portable and stable after desktop theme changes. Store font choices too; report substitution when a font is missing. Omarchy's palette does not prescribe a presentation font. Offer to copy a chosen theme wallpaper into `images/` as a background.

## Modeling and tooling

Follow `~/Work/omacom/omacut`: C++17, Qt 6 Quick/QML with Material controls, embedded QML resources, qmake, `bin/build`, `bin/test`, `bin/install`, and an Arch package with desktop entry and icon. Reuse its portal file-picker approach and worker/process patterns where appropriate.

Keep the responsibilities small and separate:

| Component | Responsibility |
| --- | --- |
| DeckDocument | Source text, lossless source ranges, edits, undo, atomic save and external-change handling |
| SlideListModel | Ordered slides, selection, thumbnail revisions, model move notifications |
| SlideLayout / SlidePainter | Text fitting, media geometry, styled text and consistent painting |
| AssetStore | Copying, naming, resolving media filenames by type, video metadata and posters |
| ThemeCatalog | Installed palette discovery and portable snapshots |
| Exporter | PDF/PPTX jobs, progress, cancellation and temporary output handling |

Single-slide edits reparse only their own source range. Preserve the other slide boundaries even while a code fence is incomplete, including across undo, redo, theme changes, and reordering. Validate the complete Markdown before saving. Before replacing an existing presentation, atomically back up its previous bytes under `.hype-backups/`; retain 20 versions per filename and abort the save if backup creation fails.

QML owns interaction and controls. C++ owns document operations and layout. Paint static slide content with QPainter through a QQuickPaintedItem, and use that same painter/layout for image and PDF output. Overlay a Qt Multimedia video surface in the resolved video rectangle during playback. A separate per-slide Markdown editor avoids coupling text input geometry to the fitted canvas.

Prototype this rendering path before building the whole editor: it aims to keep text geometry consistent and preserve vector text in PDF. Qt supports painting in the Quick scene through [QQuickPaintedItem](https://doc.qt.io/qt-6/qquickpainteditem.html) and PDF output through [QPdfWriter](https://doc.qt.io/qt-6/qpdfwriter.html). Verify font embedding, glyph coverage, and scaling on real fixtures rather than assuming identical output.

Virtualize the thumbnail list, cache by slide/source/theme revision, and regenerate only changed slides. Decode large images and generate video posters away from the UI thread. Validate responsiveness with a 150-slide deck and large media files.

Keep sidebar loading separate from full-preview decoding. Retain two viewports of nearby thumbnails and preload the three slides on either side after navigation settles. Give current requests priority over prefetches, discard obsolete queued work, and serve cache hits without waiting for decodes. Share bounded image caches across workers, with separate thumbnail and full-preview budgets; publish generated video posters atomically.

## Export contract

Run PDF and PowerPoint export in a separate Hype process using a snapshot of the current Markdown and palette. Keep the editor responsive, show rendering/conversion/packaging progress and cancellation in the footer, and publish the destination atomically only after success. Preserve existing files on failure or cancellation, terminate encoder children when cancelled, and remove temporary work.

PDF: one page per slide at the deck aspect ratio, with painted text and images. Videos become their poster frames. Use the same layout as the editor and embed fonts where supported by their licensing and Qt. Verify Unicode including color emoji, code, transparency, image cropping, and HiDPI rendering.

PowerPoint: a high-resolution rendering of each static slide placed edge-to-edge in a `.pptx`. Rendered slides preserve appearance and avoid font substitution. Editable PowerPoint text and elements are out of scope; all authoring happens in Hype or the Markdown source. Render at 3840 × 2160 per static slide and converted animation, disable automatic picture compression, measure file size on a 150-slide deck, and use JPEG for photographic slides where acceptable while retaining PNG for text/code. Video slides include an embedded movie and poster, positioned over the static slide rendering. Render static content without duplicating the video poster underneath where transparency/cropping would cause artifacts.

Build the PPTX container directly in C++ with Qt XML and a small ZIP writer using zlib. Scope it to Hype’s rendered slides and embedded H.264/AAC MP4 videos, including posters, fitting, overlays, and playback settings. Animated WebP/GIF images are composited with the slide background and text, converted to temporary H.264 MP4 videos, and embedded with the original autoplay and repeat settings. PDF keeps the first frame. Python and python-pptx are not runtime dependencies; the private trial import tools may continue using them. Write to a temporary destination and commit atomically. Verify ZIP/XML relationships and playback metadata, round-trip through LibreOffice, and test actual video playback in Microsoft PowerPoint; a successful export file alone is insufficient.

Export jobs use a temporary destination, report progress, permit cancellation, and publish the output only after success. No LibreOffice installation should be required just to export. PPTX export must remain self-contained after moving its output file to another machine.

## Build order and completion checks

1. **Prove parsing, rendering and export.** Parse a hand-authored fixture deck through the real source scanner covering headline, image fit/fill, background plus text, quote with attribution, hard-break line stacks, lists, headline/subtitle, simple tables, short and long code, inline code, and video. Render through Qt and export both formats. Compare to the editor; test PPTX video in PowerPoint. Treat video export as a separate early compatibility spike, but keep it required for completion of PPTX export.
2. **Build the document core.** Parse and render Markdown, add/duplicate/delete/reorder slides, preserve untouched source, undo/redo, save/open, and handle external changes. Test separators inside fences, unknown metadata, Unicode, filename resolution (including spaces, uppercase extensions, and optional directory prefixes), bracket directives and alt text, automatic posters, missing media, and reordering with references.
3. **Build the visual editor.** Left thumbnails, right slide canvas, per-slide Markdown editing, resizable preview/editor divider, drag reordering, keyboard navigation, and contextual media controls. Verify that dragging slide 5 between slides 1 and 2 moves its exact Markdown block to that position; adding and duplicating insert source blocks after selection; undo restores source and selection; and save/reopen preserves the result. Confirm selection changes and presentation mode never discard edits. Test empty slides at both document boundaries and duplication of media slides without extra asset copies.
4. **Complete media, themes, and presenting.** Import/paste/drop, playback and posters, installed theme picker and saved palettes, code highlighting, fullscreen navigation. Move a deck folder and verify it still works.
5. **Package and validate real usage.** Recreate representative slides from the 2024–2026 decks as private local fixtures. Test a 150-slide presentation, export cancellation/failure, PDFs at presentation and print scale, and self-contained PPTX playback on another machine. Provide the Omacut-style build/test/install scripts and Arch package.

Version one is complete when a talk can be authored entirely in Markdown or through visual slide operations, reordered without source loss, presented with local images and videos, themed from installed Omarchy palettes, and exported as rendered PDF/PPTX files. Keep direct canvas text editing, crash recovery, transitions, freeform object placement, collaboration, existing Keynote/PPTX import, and presenter view for later.


## More examples from the existing decks

These are proposed Hype representations of slides in `~/Dropbox/Documents/Presentations/`, not imported decks. Each code block below is a separate slide unless it contains a slide separator. Text and media names come from the cited decks; abbreviated or adapted examples are labeled. Media must be copied into the new deck's `images/` or `videos/` directory.

### Headline and subtitle

Omacon 2026, PDF page 67. The heading is dominant; the paragraph is a smaller subtitle. No layout property needed.

```markdown
# Pro Computers

For & By The People Who Love ’Em
```

### Several large points without bullets

Omacon 2026, PDF page 68. Markdown hard breaks (a trailing backslash) preserve the four lines. Fit the stack as a unit, with one shared font size.

```markdown
Pragmatic\
Ergonomic\
Aesthetic\
Sovereign
```

### Headline with multiple points

Adapted from Rails World 2025, slides 114–115, combining the section title and its following list. Keep normal Markdown bullets when bullets are wanted.

```markdown
# LESS!!!

- Native Ruby + Docker DBs
- Serving `.localhost`
- Drop system tests
- Testing with local CI
- Scripted setup
- Deploy from local
```

### Quote with attribution and emphasis

Omacon 2026, PDF page 7. Wrap the quote automatically; preserve its paragraph break. Attribution is smaller and separated below. Bold text gets the theme accent.

```markdown
> You become a man's Friend without knowing or caring whether he is
> married or single or how he earns his living. What have all these
> ‘unconcerning things, matters of fact’ to do with the real question,
>
> **Do you see the same truth?**

— C.S. Lewis, *The Four Loves*
```

### A longer quote with selective emphasis

Omacon 2026, PDF page 65. Use ordinary emphasis to pick out phrases instead of custom color markup. Text follows the original; emphasis is adapted to Hype's theme accent.

```markdown
> Part of the problem seems to be that nobody these days is content
> to merely put their dent in the universe. No, they have to
> **fucking own the universe**. It’s not enough to be in the market,
> they have to **dominate it**. It’s not enough to serve customers,
> they have to **capture them**.

— Me, A Decade Ago, *RECONSIDER*
```

### A big number with a qualifier

Rails World 2024, slide 80. The number is the headline; the qualifier is the subtitle.

```markdown
# 60,000 RPS

(on Kevin’s laptop)
```

### A compact budget

Rails World 2025, slide 60, adapted to a simple Markdown table. Column alignment comes from standard table syntax. These are figures from the historical slide, not current measurements.

```markdown
# Bootstrap Budget

| Time | Task |
| ---: | :--- |
| 5m | Install OS |
| 3m | Setup app |
| 2m | Run Local CI |
| 3m | Deploy code |
```

### Two sides of a comparison

Rails World 2025, slide 3, represented as text columns. This captures the comparison without recreating the original meme artwork. For the artwork, use its original image as a full-slide asset.

```markdown
| Single dev in 1999 | 50-strong team in 2025 |
| :--- | :--- |
| Deploys in 5s via SFTP | Deploys in 15m via CI/CD |
| From a Pentium III | From infinite cloud CPUs |
```

Limit table support to text, inline code, and emphasis in cells; no merged cells or nested layouts. These comparisons justify adding simple tables to version one, not a general freeform column editor.

### A build across successive slides

Rails World 2024, slides 32–34. Each step is a normal slide; duplicate and add the next line. This avoids an animation language. Keep the same headline size and position across the sequence.

```markdown
# Are YOU suffering from server-phobia?

---

# Are YOU suffering from server-phobia?

Don’t worry. There’s a cure.

---

# Are YOU suffering from server-phobia?

Don’t worry. There’s a cure.\
It’s called **LINUX**.
```

### Real code instead of a screenshot

An excerpt from `2025/Rails World 2025/code.rb`, with a descriptive heading added. Fit the code against both width and height, without wrapping or losing indentation.

````markdown
# Markdown responses

```ruby
class PagesController < ActionController::Base
  def show
    @page = Page.find(params[:id])

    respond_to do |format|
      format.html
      format.md { render markdown: @page }
    end
  end
end
```
````

### Full-slide artwork and a contained code screenshot

Existing assets in `2025/Rails World 2025/media/`. The first is edge-to-edge; the second preserves the screenshot beneath a headline. Copy these files into `images/`.

```markdown
![span](merchants-of-complexity.jpg)

---

# Local CI

![fit](config-ci-yml.png)
```

### Video with a filename containing spaces

The existing `2024/Rails World 2024/RW Demo Final.mp4`, copied into `videos/`. Standard Markdown angle brackets handle spaces. It plays once on slide entry, with a generated poster and no extra settings.

```markdown
![](<RW Demo Final.mp4>)
```
