# Components

Components lay out designed slides (cards, stats, charts, timelines, logo
walls, split panels) in Markdown, without drawing images. Everything stays
editable text, follows the theme, and stays sharp in PDF and PowerPoint. Never
draw a slide as SVG or PNG: compose it from these components. If one is
missing, combine primitives (`Row`, `Column`, `Grid`, `Card`, `Icon`, `Text`).

```markdown
<Eyebrow>About Omarchy</Eyebrow>
# What we believe

<Grid cols="3">
<Card icon="sparkles" title="Beautiful by default" align="center">
A desktop that looks finished the moment it boots.
</Card>
<Card icon="keyboard" title="Fast at the keyboard" align="center">
Every common action is a shortcut away.
</Card>
<Card icon="wrench" title="Yours to change" align="center">
Plain config files anyone can read and adjust.
</Card>
</Grid>
```

## Rules

- A tag starts a line. `<Name/>` closes itself, `<Name>text</Name>` fits on one
  line, and anything longer ends with `</Name>`. Short tags may follow one
  another on a line: `<Column><Spacer size="40"/><Card title="x"/></Column>`.
- Props are `name="value"`. A bare prop such as `divided` means `true`.
- Inside components, write ordinary Markdown: `**bold**` (shown in the accent
  color), lists, tables, and `### headings`. Indentation is optional.
- `# Title {align=center}` sets props on one heading: `align`, `tone`, `size`.
- Ordinary line breaks stay visible, as on every Hype slide.
- Don't position things with coordinates. Rows, columns, and grids size
  themselves, and the slide scales one fit factor to its content: dense slides
  shrink, sparse ones grow. `hype check` warns when text gets too small. Then
  say less or split the slide.
- `hype check` names every unknown component, prop, color, icon, or image, and
  suggests the closest valid name.
- Look at your work: `hype render deck.md --slide N -o /tmp/slide.png`.

Slide settings go in a comment anywhere on the slide:
`<!-- hype: justify="center" fit="shrink" frame="none" -->`. By default
(`justify="auto"`), the title stays at the top and the rest is centered below
it. `fit="shrink"` never enlarges content. `frame="none"` leaves out the
template frame.

## Values

| Kind | Values |
| --- | --- |
| Colors (`tone`, `fill`, `ring`, `border`) | `accent`, `ink` (text color), `muted`, `surface` (card), `background`, `line`, `gradient`, `green`, `red`, `yellow`, `magenta`, `cyan`, any `color_*` key in the front matter, `white`, `black`, or `#rrggbb` |
| Fills | Any color, `gradient` (the theme's), or two colors for a gradient: `fill="purple pink"` with `angle="135"` (CSS degrees). `Text` takes these as its `tone` too, for gradient lettering |
| Sizes (`size` on text) | `xs` 18, `sm` 22, `md` 26, `lg` 32, `xl` 40, `2xl` 52, `3xl` 64, `4xl` 80, `5xl` 104, or pixels on a 1920×1080 slide |
| Spacing (`gap`, `pad`) | `none`, `xs`, `sm`, `md`, `lg`, `xl`, or pixels. `pad="24 40"` is vertical then horizontal; four values go top, right, bottom, left |
| Width | `40%`, `320px`, or `auto` (fit the content). In a `Row`, a bare number is a share: `width="2"` takes twice the room of `1` |
| `align` | `start`, `center`, `end`, `stretch` (`left`, `right` work too). In a column it aligns across and sets text alignment; in a row it aligns vertically |
| `justify` | `start`, `center`, `end`, `between`, `stretch` along the main direction |
| Icons | Any [Lucide](https://lucide.dev/icons) name. `hype icons <word>` searches, for example `hype icons chart` |
| Fonts (`font`) | `heading`, `body`, a type role (`title`, `number`, `eyebrow`), a `font_<name>` alias from the front matter, or any family name. Google Fonts families download on first use |

Every component takes `width`, `height`, `grow` (take the remaining room,
optionally weighted: `grow="2"`) and `align-self`. Every component also takes
`font`, which sets the family for all the text inside it, `bold`, the
weight of **bold** text inside it (`bold="black"` or `bold="900"`), and
`bold-tone`, its color. For example, `<List font="heading" bold="black">` sets
a list in the heading font with black lead-ins, and `<Text tone="white"
bold-tone="accent">What is the **one big thing**?</Text>` colors two words.
Bold words in body text take the accent unless `bold-tone` says otherwise; in
`#` headings they only change color when `bold-tone` is set. A component's own
titles and numbers follow a `font` set on it. On a `Row`, `Column`, or
`Grid`, `height` is a minimum: the box grows to hold its content. A box that
holds something growing, such as a chart, grows with it. Boxes also take
`clip` to crop their content to their rounded corners.

## Layout

### Row, Column, Grid

```markdown
<Row gap="40" align="center">
<Column width="2">…</Column>
<Column>…</Column>
</Row>

<Grid cols="4" gap="24">
…eight cards…
</Grid>
```

- `Row`: children side by side. Equal shares by default; `align="stretch"`
  makes children equally tall.
- `Column` (or `Stack`): children top to bottom. `justify` spreads them.
- `Grid`: equal cells, `cols` per row. An incomplete last row is centered
  unless `balance="false"`. Add `grow` to stretch the rows to fill the slide.
- Containers can be decorated: `fill`, `pad`, `radius` (pixels or `theme`),
  `border`, `shadow`.

### Split and Panel

Full-bleed panels edge to edge: a colored side, a photo half, a sidebar.

```markdown
<Split ratio="40:60">
<Panel fill="gradient">
<Eyebrow tone="white">Case study</Eyebrow>
# Moving a studio in one weekend
</Panel>
<Panel image="desk.jpg" grayscale overlay="0.5"/>
</Split>
```

`Split` takes `ratio` (`40:60`, `1:2:1`) and `direction="column"` for
horizontal bands. A `Panel` takes `fill`, `image` (cover), `overlay` (0 to 1),
`grayscale`, `position` (`top`, `bottom`, `left`, `right`), `pad` (default 72),
`gap`, `align`, and `justify` (default `center`). Text on a fill or picture
switches to a contrasting color automatically.

### Background and Place

```markdown
<Background image="city.jpg" grayscale overlay="0.4"/>
<Background band="bottom" size="45%" fill="gradient"/>
<Place at="top-right"><Image src="logo.png" height="40"/></Place>
<Place at="bottom" inset="0" height="10" fill="gradient"/>
```

- `Background` paints behind the slide: `image`, `fill`, `overlay`,
  `grayscale`, `blur`, `position`. `band="top|bottom|left|right"` with
  `size="45%"` paints only that strip. Content over a band reads against it.
- `Place` pins decoration to `at`: `top-left`, `top`, `top-right`, `left`,
  `center`, `right`, `bottom-left`, `bottom`, `bottom-right`. `inset` is the
  distance from the edges (default `44 56`); `width`, `height`, and `fill` size
  and color it. Placed things never shrink with the content, and titles keep
  clear of a logo in the top corner.

### Spacer and Divider

`<Spacer/>` takes the remaining room (`size="40"` fixes it). `<Divider/>` is a
hairline: horizontal in a column, vertical in a row. It takes `tone` and
`thickness`.

## Content

### Text and Eyebrow

```markdown
<Eyebrow>Plan & recommendation</Eyebrow>
<Text size="xl" weight="600" tone="accent" align="center">A quick, bold statement.</Text>
```

`Text` takes `size`, `tone`, `align`, `weight` (`400`–`900`, `medium`,
`bold`, `black`), `font="heading"`, `uppercase`, `tracking` (percent), `italic`,
and `line` (line height, such as `1.1`). `tone="gradient"` (or two colors, such
as `tone="purple pink"`) fills the letters with a gradient. `rotate="-90"` or
`"90"` turns the text to run up or down the slide, for a label beside a grid or
a group. `Eyebrow` is the small uppercase label above a title: `tone`, `align`,
`size`.

### Card

```markdown
<Card icon="rocket" title="Launch" tone="accent">Body Markdown.</Card>
<Card number="1" title="Audit">…</Card>
<Card header="tab" title="Clear communication" align="center">…</Card>
<Card header="pill" title="The architecture" fill="dark">…</Card>
<Card image="team.jpg" title="Human centric">…</Card>
<Card accent="top" tone="green" title="Always-on optimization">…</Card>
```

| Prop | Meaning |
| --- | --- |
| `title`, `icon`, `number`, `image` | What goes above the body. `number` is a colored square badge. `image` fills the top (`image-height` sets its height) |
| `icon-style` | `plain`, `circle`, `soft`, `square`, `outline` |
| `icon-size` | `sm`, `md`, `lg`, `xl`, `2xl`, `3xl` |
| `header` | `tab`: the title in a dark band across the top. `pill`: the title in a pill on the top edge |
| `accent` | A colored edge: `top`, `left`, `bottom` |
| `tone` | Color of the icon, number, accent, tab or pill |
| `title-tone` | Color of the title |
| `fill`, `border`, `shadow`, `radius`, `pad`, `gap` | The surface. Cards are white with a soft shadow on light slides and outlined on dark ones |
| `align`, `justify`, `text-size` | Content alignment and body size (default `sm`) |

### Stat

```markdown
<Stat value="420" label="Desks on Omarchy" delta="+18% this quarter" trend="up"/>
<Stat value="28 min" label="Setup time" delta="61% faster" trend="down" good="down" layout="row"/>
```

`value`, `label`, `caption`, `delta`, `trend` (`up`, `down`, `flat`: draws an
arrow), `good` (which direction is good, default `up`; this colors the arrow and
delta green or red), `icon`, `tone` (value color, default accent), `size`
(`sm`, `md`, `lg`, `xl`), `layout="row"` (value beside the label), and `align`.

### List and Tags

```markdown
<List icon="check" icon-style="box" divided>
- **Encrypted disks** Full-disk encryption on every machine
- :shield: **Security** Automatic updates and a firewall by default
</List>

<Tags>
- :map-pin: Designers
- Developers
</Tags>
```

`List` turns Markdown items into rows with a marker. Use `icon`,
`icon-style` (`box`, `circle`, `soft`, `plain`, `number`, `dot`), `tone`,
`cols`, `gap`, `size`, `divided` (hairlines between items), and `connected` (a
line through the markers, for a sequence). Start an item with `:icon-name:` to
give it its own icon; a leading `**bold**` phrase becomes its title. Text on the
next line, indented, goes under the title instead of after it. `Tags` makes a
wrapping row of pills (`tone`, `variant`, `size`).

### Pill, Callout, Quote

```markdown
<Pill tone="gradient">AI embedded throughout</Pill>
<Callout icon="circle-alert" title="Outcome:">We exceed the 2028 goal a year early.</Callout>
<Quote by="Alex Rivera" role="Provost" image="alex.jpg">Online learning is the expectation.</Quote>
```

- `Pill` (or `Badge`, `Tag`): `tone`, `variant` (`solid`, `soft`, `outline`,
  `square`), `icon`, `size`.
- `Callout`: a bar with an icon. `title` is a bold lead-in. `fill` defaults to
  the text color (dark on light slides), and `tone` colors the icon.
- `Quote`: a large quote mark, the quote, and `by`, `role`, and `image`.

### Image, Person, Logos, Icon

```markdown
<Image src="desk.jpg" aspect="16:9" radius="theme"/>
<Image src="logo.png" height="48" trim/>
<Person image="avery.jpg" name="Avery Chen" role="VP of Strategy" ring="accent"/>
<Logos cols="5" tone="white">
![](logos/acme.png)
![](logos/globex.png)
</Logos>
<Icon name="rocket" size="xl" style="circle" tone="accent"/>
```

- `Image`: `src` from `images/`, `fit` (`cover` or `contain`), `shape` (`rect`,
  `rounded`, `circle`), `size` (a square), `height` (width follows the
  picture), `aspect` (`16:9`), `radius`, `ring` (a colored outline),
  `grayscale`, `position`, `shadow`. `trim` crops transparent or white margins,
  which is useful for logos. `play` adds a play button, for a still that stands
  for a video elsewhere. In a row beside text, a picture crops to the text's
  height.
- `Person`: portrait, name, role, and `caption`. `layout="row"` puts the
  portrait beside them; `size`, `ring`, and `shape` style the portrait.
- `Logos`: a grid of logos sized to look equally heavy, with margins trimmed.
  `tone` is `original`, `white`, `black`, or `ink`. Also `cols`, `size` (cell
  height), `gap`.
- `Icon`: `name`, `size` (`xs`–`3xl` or pixels), `tone`, `style` (`plain`,
  `circle`, `soft`, `square`, `outline`), `stroke` (line width, default 1.75).

### Video, Collage, QR

```markdown
<Video src="demo.mp4" poster="still.jpg" radius="theme" autoplay="false"/>
<Collage rows="1,2">
![](office.jpg)
![](nurse.jpg)
![](family.jpg)
</Collage>
<QR value="https://example.com/watch" size="200" label="Scan to watch"/>
```

- `Video`: a video from `videos/`, played in place while presenting and
  embedded where it sits in PowerPoint. Thumbnails, PDF, and previews show its
  first frame, or `poster`, with a play button (`play="false"` hides it).
  Also `aspect`, `radius`, `shadow`, `autoplay`, `loop`, and `muted`. A slide
  plays its first video.
- `Collage`: photos cropped into tidy rows that fill the space, each keeping
  roughly its proportions. `rows="2"` balances them over two rows, and
  `rows="1,2"` sets each row's count. Also `gap`, `radius`, and `grayscale`.
- `QR`: a QR code for `value`, on a white square so phones can read it on any
  slide. Also `size`, `tone` (a dark color for the modules), `label`, and
  `align`.

### Table

Any Markdown table on a component slide is styled: a filled header row, zebra
rows, and comfortable padding. Wrap it for options:

```markdown
<Table header="accent" highlight="last">
| Item | Fee | Model |
| --- | --- | --- |
| Discovery | $120,000 | Fixed |
| Totals | $15,000,000 | |
</Table>
```

`header` is the header color (default: text color), `zebra="false"` drops the
stripes, and `highlight="last"` marks a totals row.

## Charts and diagrams

Data lives in the slide as a Markdown table or list, so it stays editable.
Charts and circular diagrams grow into free room; set `height` to fix them.

### Chart

```markdown
<Chart type="column" goal="400" goal-label="Rollout goal">
| Quarter | Desks |
| --- | --- |
| Q3 25 | 260 |
| Q4 25 | 340 |
| Q1 26 | 420 |
</Chart>
```

The first column holds categories; each further column is a series. Cells may
read `$1.2M`, `42%`, or `7,614`, and the label shows the cell as written.

| Prop | Meaning |
| --- | --- |
| `type` | `column` (default), `bar` (horizontal), `stacked-column`, `stacked-bar`, `line`, `area`, `donut`, `pie` |
| `colors` | Series colors in order, such as `colors="accent, cyan"`. Default: `chart_colors` from the front matter, else the theme's |
| `labels` | `auto` (value at each bar end, the last point of a line), `all`, `end`, `none` |
| `legend` | `auto` (shown for two or more series), `inline` (each line named at its start), `none` |
| `goal`, `goal-label` | A dashed target line |
| `marks` | Labelled lines at categories: `marks="Q2 26: Pilot ends; Q4 26: Rollout complete"` |
| `highlight` | One category in color, the others muted |
| `min`, `max` | Axis range; lines start near their data unless `min` is set |
| `smooth` | Lines curve unless `smooth="false"` |
| `size` | `sm`, `md`, `lg` for the label text |

Compare a few values with a column or bar chart, and change over time with a line.
Use a donut only for parts of a whole with at most six slices. For a single
number, use a `Stat` instead.

### Progress

```markdown
<Progress value="72" label="Lead goal" show="percent"/>
<Progress tone="green">
| Program | Leads | Goal |
| --- | --- | --- |
| Nursing | 640 | 800 |
| Business | 512 | 600 |
</Progress>
```

### Gantt

```markdown
<Gantt today="2026-07-01" labels="inside">
| Task | Start | End | Phase |
| --- | --- | --- | --- |
| Discovery | 2026-05 | Jun | Phase 1 |
| Campaign production | Jun | Aug | Phase 2 |
| Launch | Sep |  | Phase 2 |
</Gantt>
```

Dates can be ISO (`2026-05-14`), months (`May`, `May 2026`, `2026-05`),
quarters (`Q3 2026`), weeks (`W3`), or plain numbers. A missing year follows
the last one given, and an empty End makes a milestone diamond. Rows are
grouped and colored by `Group`/`Phase`; a `Tone` column overrides a row's
color. Props: `start`, `end`, `today`, `labels` (`left`, or `inside` to put
names in their bars), `groups` (`top`, or `side` for a turned label beside each
group), and `colors`.

### Timeline, Steps, Cycle, Funnel, Venn, Tree

Each reads a Markdown list: `- :icon: ![](picture.png) **Title** Text`; every
part is optional. A second line indented under an item continues it.

```markdown
<Timeline>
- **2023** Go keyboard-first
  Shortcuts for everything people do daily.
- **2025** Reset the setup
  An Omarchy pilot on twelve desks.
</Timeline>

<Steps style="chevron">
- :search: **Audit** Understand every machine
- :rocket: **Activate** Roll out and improve
</Steps>

<Cycle style="wheel" center="One setup">
- :code: **Ownership** Clear accountability
- :link: **One look** Every machine alike
- :folder: **Dotfiles** One source of truth
</Cycle>

<Funnel>
- **Trial** 120 pilot desks
- **Adoption** 420 desks
</Funnel>

<Venn center="Flow">
- Hardware
- Software
- People
</Venn>

<Tree>
- ![](avery.jpg) **Avery Chen** Chief Executive
  - ![](blake.jpg) **Blake Ortiz** Design
  - **Casey Park** Engineering
</Tree>
```

- `Timeline`: `direction` (`horizontal`, `vertical`), `style` (`dot`,
  `number`, `icon`), `tone`, `colors`.
- `Steps`: `style` (`chevron`, `arrow` (cards joined by arrows), `card`,
  `circle`), `direction="vertical"`, `numbered="false"`, `tone`, `colors`.
  `hub="AI embedded throughout"` joins every step to one labelled pill below
  (`hub-tone` colors it).
- `Cycle`: `style` (`ring`, or `wheel` for colored segments), `center`,
  `center-image`, `numbered`, `colors`. On a ring, `phases="Explore, Establish,
  Execute"` sets words around an inner circle.
- `Funnel`: stages narrow downward with each title inside. An item's text
  goes beside its stage; `labels="inside"` puts it in the stage under the
  title. `axis="Low awareness; High awareness"` labels a scale beside it.
- `Venn`: two, three, or four circles (four make a flower), with an optional
  `center` label or `center-image`.
- `Tree`: an org chart or hierarchy. Indent each person under the person
  they belong under. Boxes show a portrait when the item has a picture, and
  initials otherwise (`avatars="none"` drops them). Connectors join each parent
  to its children. If there are no reporting lines to show, use a `Grid` of
  `Person` cards instead.

All of them take `size` (`sm`, `md`, `lg`) to scale their text.

## Templates

A template carries a brand: colors, fonts, assets, and your own components
for slides you make again and again. Any presentation can be a template, so
the deck you made last time can start the next one. Choose it in the front
matter:

```markdown
---
title: "Proposal"
template: acme
---
```

`template:` names a template, a folder, or a presentation:

- `acme` looks in `templates/acme/` beside the presentation, then `acme/`,
  then your template library in `~/.local/share/hype/templates/acme/`, then
  the templates Hype comes with, such as `omarchy`.
- `../last-pitch` or `~/Brand/acme` is a folder.
- `../last-pitch/pitch.md` is a presentation.

In a folder, Hype reads `template.md`, or else `presentation.md`:

```text
templates/acme/
  presentation.md   front matter and slides, with <Define> blocks (or template.md)
  fonts/            .ttf/.otf files, loaded automatically
  images/           logos and pictures; the presentation's own images/ comes first
```

A presentation built on a template takes its front matter (all but `title`),
every `<Define>` on its slides, its fonts, and its images. The template's
slides stay behind, as slides a new one can start from. If the template names
a `theme:`, its colors come too. A `theme:` the presentation names itself
recolors the template instead. A template can name a `template:` of its own:
Hype follows the chain, and the nearer template wins each setting and
component.

### The template library

```text
hype templates                                  every template you can name
hype template save brand/presentation.md        a link that follows the file
hype template save brand/presentation.md --name acme-2026 --copy
hype template remove acme-2026
```

`hype template save` adds a presentation to your library under its folder's
name (or `--name`). A link follows the presentation as you keep editing it,
and reads its pictures and fonts from beside it. `--copy` keeps it as it is
now, with its `images/` and `fonts/`. The editor does the same from **File ›
Save as template…**, lists every template under the palette icon, and adds a
slide that starts as one of the template's own with **Ctrl+Shift+Enter**.

### A slide library

A template's slides are ready-made slides to copy: the team's bios, case
studies, the slides every pitch opens with. Group them into sections: a
section starts on the slide that names it, and runs to the next one.

```markdown
<!-- hype: section="Case study · Acme" -->
```

The editor's **New slide from template** (Ctrl+Shift+Enter) shows the slides
under their sections, with a search, and adds a single slide or a whole
section. A copy leaves behind the template's definitions, the comment directly
above each `<Define>` that documents it, and the section mark.

```text
hype template slides omarchy          the slides, by section, numbered
hype template slides omarchy 3        slide 3's Markdown, ready to paste
hype template slides omarchy --json   every slide's section, label, and Markdown
```

Hype comes with `omarchy`: the desktop's own colors (`theme: current`),
JetBrains Mono, and square tiles like Hyprland's windows, with `Title`,
`Section`, `Window`, `Terminal`, and `Keys` components.

### Template front matter

```markdown
---
font: "Inter"
font_heading: "Fraunces"
heading_weight: 800
radius: 20
margin: "64 88"
frame: Frame
color_background: "#f4f2ee"
color_foreground: "#16161a"
color_accent: "#4f46e5"
color_coral: "#f97362"
color_gradient_start: "#4f46e5"
color_gradient_end: "#f97362"
chart_colors: "accent, coral, cyan, yellow"
---
```

Every `color_name` becomes a color you can use as `tone="name"`. The
presentation's own front matter overrides the template's.

Fonts work the same way: `font` is the body, and `font_heading` is for titles.
Any other `font_<name>` is an alias that components can use, such as
`font_condensed: "Encode Sans Condensed"` with `font="condensed"`. Type roles
set the weight (and optionally the family) of what components draw:

| Role | Used for | Family | Weight |
| --- | --- | --- | --- |
| heading | `#` titles, `font="heading"` | `font_heading` | `heading_weight` (700) |
| title | card titles, person names, tabs | `font_title`, else heading | `title_weight`, else heading |
| number | stat values | `font_number`, else heading | `number_weight`, else heading |
| eyebrow | `Eyebrow` labels | `font_eyebrow`, else body | `eyebrow_weight` (700) |

Text can name a role directly: `<Text font="number">$2 MM+</Text>` or
`weight="title"`. Put the template's font files in its `fonts/` folder with
`hype fonts add "Fraunces" --into templates/acme/presentation.md`. Other settings are
`margin` (slide margins, vertical then horizontal), `radius` (corners for
`radius="theme"`, cards and images), `gradient_angle`, `fit` (`shrink` never
enlarges), and `fit_max` (the largest growth, default 1.4).

### Your own components

Write a `<Define>` on any slide. It draws nothing there; it adds a component
that every slide of the presentation can use, and so can every presentation
built on it. A presentation's own definition replaces a template's of the
same name. Put a definition beside a use of it, so its slide shows what it
draws.

```markdown
<Define name="Section" image="desk.jpg" subtitle="" frame="none">
<Background image="{image}" grayscale/>
<Background band="left" size="30%" fill="gradient"/>
<Column justify="end" grow>
<Card width="50%" align="center">
<Text font="heading" size="60">{children}</Text>
<Text size="sm">{subtitle}</Text>
</Card>
</Column>
</Define>
```

Use it like any component:

```markdown
<Section image="office.jpg" subtitle="Our rollout plan">
How we work
</Section>
```

- `{prop}` is replaced by the prop's value. Attributes on `Define` are
  defaults, and a prop left empty removes the text, pill, or image that uses it.
- `{children}` is everything inside the tag.
- For more than one block of content, use named slots. Write
  `<Slot name="logos">…</Slot>` inside the use, and `{logos}` in the
  definition receives it.
- `{page}` and `{pages}` give page numbers, and `{deck-title}` the title.
- `name` and `frame` are reserved on `Define`. `frame="none"` (or another
  frame's name) applies whenever the component is the slide's first element.

### The frame

`frame: Frame` in the front matter wraps every slide in that definition, so
logos, page numbers, and brand strips appear everywhere:

```markdown
<Define name="Frame">
<Place at="top-right"><Image src="logo.png" height="36" trim/></Place>
<Place at="bottom-right"><Text size="xs" tone="muted">{page}</Text></Place>
<Place at="bottom" inset="0" height="10" fill="gradient"/>
{children}
</Define>
```

Plain Markdown slides take the frame too. Picture, gallery, and word cloud
slides keep their own layouts. `<!-- hype: frame="none" -->` leaves the frame
off one slide.

`hype new talk/presentation.md --template acme` starts a presentation on a
template.
