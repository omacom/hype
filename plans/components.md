# Components: composable slides in Markdown

## Why

Each richer slide used to need its own layout keyword in C++ (`split`, `caption`,
`caption-right`, `gallery`, `cloud`). Anything else, such as card rows, stats,
timelines, charts, or logo walls, sent agents off to draw SVGs or matplotlib PNGs.
Those images can't be edited, ignore the theme, and blur in the PDF.

Components replace that with a small vocabulary of layout containers and
primitives, written as MDX-style tags inside an ordinary slide. Theme tokens
drive their look. Templates bundle brand colors, fonts, assets, and reusable
slide layouts (user-defined components).

## Principles

- **Markdown first.** Text inside a component is Markdown. Data comes from
  Markdown tables and lists, so it stays editable in either editor mode.
- **Agent-friendly.** PascalCase tags with `key="value"` props, as in MDX and JSX.
  Every component and prop is validated: `hype check` names the problem and
  suggests the closest valid name. `hype help components` prints the reference;
  `hype icons <query>` searches the bundled Lucide icons.
- **No coordinates.** Rows, columns, grids, and splits size themselves; one fit
  factor per slide keeps the type scale consistent while content shrinks to fit.
  `<Place>` positions only decoration (logos, page numbers, strips).
- **Theme tokens, not colors.** `tone="accent"`, `fill="surface"`,
  `fill="gradient"`. Hex works when needed. Text on a fill picks a contrasting ink.
- **Vector all the way.** Shapes, icons, and charts are painted with QPainter, so
  the PDF stays sharp and the PPTX slides render at 4K.
- **Old decks unchanged.** A slide becomes "composed" only when it contains a
  component tag (or a template frame applies to plain text slides).

## Syntax

```markdown
<Eyebrow>About Omarchy</Eyebrow>
# What we believe

<Grid cols="3">
<Card icon="sparkles" title="Beautiful by default">
A desktop that looks finished the moment it boots.
</Card>
<Card icon="keyboard" title="Fast at the keyboard">
Every common action is a shortcut away.
</Card>
<Card icon="wrench" title="Yours to change">
Plain config files anyone can read and adjust.
</Card>
</Grid>
```

- Tags start a line. `<Name …/>` is self-closing; `<Name …>inline</Name>` works
  on one line; otherwise content runs until `</Name>` on its own line.
- Children may be indented; Markdown chunks are dedented.
- Code fences and `<!-- comments -->` are opaque.
- Data components (`Chart`, `Gantt`, `Timeline`, `Steps`, `List`, `Logos`, …)
  read their inner Markdown table or list directly.

## Templates

`template: brand` in front matter loads `templates/brand/template.md` beside the
presentation (or `~/.local/share/hype/templates/brand/template.md`, or a path
ending in `.md`). A template file has:

- Front matter: colors, fonts, radius, and `frame`. The presentation's own front
  matter overrides it.
- `<Define name="Section" eyebrow="">…</Define>` blocks: user components. Props
  substitute `{name}`; `{children}` is the usage's inner Markdown. Defaults come
  from the `Define` attributes.
- `fonts/` and `images/`, loaded and resolved automatically.

`frame: Frame` wraps every slide in the named definition, for logos, page
numbers (`{page}`, `{pages}`), and brand strips. `<!-- hype: frame="none" -->`
opts a slide out.

## Engine

`src/blocks.cpp` parses, expands, validates, lays out, and paints.
`src/diagrams.cpp` paints charts and diagrams. `src/icons.cpp` and
`src/icons.tsv` provide the icons.

1. Parse the slide into a node tree with slide-relative line numbers.
2. Expand template components (textual substitution, then reparse; depth-limited).
3. Validate names, props, enums, assets, and icons.
4. Lay out at fit factor `s`: `measure(width) → height`, then `place(rect)`.
   Binary-search `s` in [0.5, 1], cached per slide source and palette.
5. Paint: background, flow, then `Place` overlays.

The canvas is 1920×1080; margins stay constant, while type, gaps, and padding
scale with `s`.
