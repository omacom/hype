# Writing a Hype presentation

A presentation is one Markdown file. Edit it with any text tool, then use
`hype check`, `hype render`, and `hype export` to verify and ship it. If the
file is open in the Hype editor, saved edits appear there as long as the editor
has no unsaved changes of its own.

## Layout on disk

```text
my-talk/
  presentation.md
  images/      photos, diagrams, animated GIF/WebP, SVG
  videos/      mp4, m4v, mov, webm, mkv
```

Media is referenced by filename only: `![](city.jpg)` reads `images/city.jpg`
and `![](demo.mp4)` reads `videos/demo.mp4`.

## Front matter

```markdown
---
title: "My talk"
theme: tokyo-night
font: "JetBrains Mono"
---
```

`hype new` writes this for you, along with the theme's `color_*` values.
`hype themes` lists the installed themes. Any `color_*` value can be
overridden with a `#rrggbb` color: `color_background`, `color_foreground`,
`color_accent`, `color_green`, `color_red`, `color_yellow`, `color_magenta`,
`color_cyan`, `color_dark_foreground`.

## Slides

Separate slides with a line holding only `---`, with a blank line on either
side. Slides are 16:9 and text is sized to fit, so say less per slide: a
headline, a few bullets, or one short quote. `hype check` warns when a slide
holds so much that its text shrinks below a readable size.

````markdown
# A big idea

---

# Keep it simple

- Write in Markdown
- Tell your story

---

> Make something wonderful.

---

```ruby
puts "Code is highlighted when you name the language"
```
````

- `# Headline` is big. Lists, quotes, tables, and inline `code` work.
- `*asterisks*` are italic, `**double**` is bold, `_underscores_` underline.
- Ordinary line breaks stay visible on the slide.
- `<!-- comments -->` are hidden from the slide; use them for speaker notes.
- A `---` inside a code fence does not split the slide.

## Word clouds

Use `<!-- hype: layout="cloud" -->` for a text-only word cloud. The first
`# Heading` is the title. Each `##` through `######` heading is one label:
fewer hashes make it larger and bolder. Labels use the theme colors, stay
horizontal, and pack automatically without overlapping. The source order
breaks ties between equal-sized labels. An optional final `> Quote` is a footer.

```markdown
<!-- hype: layout="cloud" -->
# A few good ideas

## Make something
### Learn something
#### Share it
##### Ask questions
###### Start small

> Work in progress
```

Change the labels or their heading levels in either editor mode; no image
generation is involved. This layout accepts a title, label headings, and an
optional footer. Use a separate slide for lists, tables, code, or media.

## Galleries

Use `<!-- hype: layout="gallery" -->` to group several images under editable
labels. Start with a `#` title, then pair each `##` label with one image.
An optional final `>` paragraph sits below the gallery. The layout fits up to
nine groups into three columns, centers the last row, and preserves image
proportions. Labels use regular-weight text unless you explicitly add bold.

```markdown
<!-- hype: layout="gallery" -->
# Our teams

## Core
![](core-portraits.png)

## Design
![](design-portraits.png)

> More coming soon
```

Gallery images are stills. Use plain image references without layout, overlay,
or video options; put each caption above its image. Edit the labels and image
references directly in Markdown.

## Images and video

Outside a gallery, each slide takes one image or video. Options go inside the brackets:

| Markdown | Result |
| --- | --- |
| `![](diagram.png)` | Show the whole image |
| `![span](photo.jpg)` | Fill the slide, cropping as needed |
| `![fit](photo.jpg)` | Show the whole image, with any text overlaid |
| `![layout=title](chart.svg)` | Put the first `# Heading` above the media; remaining text overlays it |
| `![layout=split](art.png)` | Put editable text on the left and sharp media on the right |
| `![layout=caption](portraits.png)` | Put an editable caption or table below sharp media |
| `![layout=caption-right](collage.png)` | Show sharp media above an editable caption under its right half |
| `![fit background=#ffffff](diagram.png)` | Fill the space around it with a color |
| `![fit background=blur](portrait.jpg)` | Fill it with a blurred copy of the image |
| `![fit background=auto](portrait.jpg)` | Match the image's edge color |
| `![overlay=0.5](photo.jpg)` | Darken the picture behind text, from 0 to 1 |
| `![loop muted](demo.mp4)` | Loop a video without sound |
| `![autoplay=false](demo.mp4)` | Wait for Space to play the video |
| `![poster=still.png](demo.mp4)` | Show an image from `images/` until it plays |

By default, text on an image slide is overlaid in white over a slightly darkened picture.
An image with a headline spans the slide unless you say `fit`.

Use `layout=title` for a chart with an editable title above it. The media fits
below the title and stays in the same position when you add stats beneath the
heading. A title alone leaves the image sharp and undimmed. Add `overlay=0.65`
to darken the whole slide backdrop beneath the title and stats. The Layout menu
offers the same choice.

Use `layout=split` for an episode card, portrait, or product image with text
beside it. Text is left-aligned in the left column; media fits in the right
column without blurring or automatic dimming. The slide uses the theme colors
unless you choose a background explicitly. `span` crops media within its column,
and an explicit `overlay` darkens only the media column. The Layout menu calls
this **Text beside media**.

Use `layout=caption-right` for a wide image with a caption beneath its right half.
Media fits above the caption and stays sharp, with no automatic dimming. The
caption is ordinary Markdown, centered within the right column and using the
theme colors. `span` crops within the media area; an explicit `overlay` dims only
the media. The Layout menu calls this **Caption below right half**.
An optional first `# Heading` appears above the media; the remaining text stays
in the caption area below its right half.

Use `layout=caption` for a caption across the full width below media. A Markdown
table in the caption has equal-width, centered columns, useful for editable names
and roles beneath a row of portraits. An optional first `# Heading` goes above
the media. Media stays sharp and undimmed by default. The Layout menu calls this
**Caption below media**.

## Commands

```text
hype new talk/presentation.md --title "My talk" --theme tokyo-night
hype check talk/presentation.md --json     every problem, with slide and line
hype slides talk/presentation.md --json    an outline of the slides
hype render talk/presentation.md --slide 3 -o slide.png
hype render talk/presentation.md -o slides/        every slide, plus slides.json
hype export talk/presentation.md talk.pdf          or talk.pptx
```

Commands need no display. They exit 0 on success and 1 on failure, with errors
on stderr. Render a slide and look at the PNG to judge how it reads.
