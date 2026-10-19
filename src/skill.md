---
name: hype
description: |
  Make and edit slide presentations with Hype, which turns one Markdown file into
  slides, PDF, and PowerPoint. Use for ANY request to create, change, review, or
  export a presentation, deck, slides, talk, or keynote, and for any .md file
  that is a Hype presentation.
---

# Hype

A Hype presentation is one Markdown file with `images/` and `videos/` beside it.
You write the file with your ordinary editing tools; the `hype` command starts,
checks, renders, and exports it. No command needs a display.

Run `hype help format` first. It prints the whole slide format, including front
matter, slide separators, and image and video options, for this version of Hype.
For designed slides (cards, stats, charts, timelines, roadmaps, logo walls,
split panels, brand templates), run `hype help components` too.

**Never draw a slide, chart, or diagram as SVG or PNG.** Compose it from
components (`Row`, `Grid`, `Card`, `Stat`, `Chart`, `Gantt`, `Steps`, …). They stay
editable, follow the theme, and stay sharp in PDF and PowerPoint. Use pictures
only for photos, screenshots, and logos.

## Workflow

1. Start: `hype new talk/presentation.md --title "My talk"` (`hype themes` lists
   themes for `--theme`). For an existing presentation, `hype slides <file>`
   prints an outline with each slide's number and lines.
2. Edit the Markdown file directly. Put media in `images/` or `videos/` and
   reference it by filename only.
3. Check: `hype check <file> --json` reports every problem with its slide and
   line. Fix errors, and treat a cramped-text warning as a cue to say less or
   split the slide.
4. Look: `hype render <file> --slide N -o /tmp/slide.png`, then view the PNG.
   Do this for slides whose layout matters: images, code, long text.
5. Export when asked: `hype export <file> talk.pdf` or `talk.pptx`.

## Designed decks

For a pitch, proposal, or any deck that should look designed:

1. Read before writing: `hype help components`, the template (its
   `template.md` or `presentation.md`), and `ls -R images/` so every picture
   you name exists. Read the brief or
   notes you are given and use their facts, names, and numbers. Never invent
   clients, results, quotes, or people.
2. Plan the arc, a few words per slide: an opening statement, who we are, what
   we do, proof (results, case studies, clients), how we work, the team, and a
   closing ask. Twelve to sixteen slides suit a first pitch.
3. Give each slide one job, and vary the layouts so no two neighbors look
   alike: full-bleed `Split` panels and photos, a row of `Stat`s for numbers,
   `Steps`, `Timeline`, or `Cycle` for a process, `Logos` for clients, `Person`
   cards or the template's team components for people, a `Chart` for data, a
   `Quote` for praise, and dark slides between light ones. Use the template's
   own components wherever they fit.
4. Build in passes a person can watch: write the front matter and the first
   few slides, save, then add a few slides at a time. The editor shows every
   save at once.
5. After each pass, run `hype check <file> --json` and fix every error and
   warning.
6. Look at every slide: `hype render <file> -o <dir>` writes one PNG per slide.
   View them and fix crowding, overflow, empty space, and weak hierarchy, then
   render the slides you changed again.

## Working with the person

- If they have the presentation open in the Hype editor, your saved edits appear
  there at once and the editor stays on the slide they are viewing. `hype open
  <file>` opens the editor for them; it needs their desktop, so do not wait on it.
- Slides are for an audience: a headline, a few bullets, or one image each. Prefer
  more slides over fuller ones. Component slides can carry more: a title with one
  row of cards, a chart, or a table. Render them and look.
- Fonts: name any installed or Google Fonts family (`font: "Poppins"`, or
  `font="…"` on a component). Hype downloads Google fonts itself; never draw text
  into images to get a typeface. `hype fonts <file>` shows where each font comes
  from. Before sharing a presentation, bundle its fonts with
  `hype fonts add "<family>" --into <file>`.
- When a presentation has a `template:`, read it: `hype templates --json`
  gives each template's file, components, and slides. Any presentation can be
  a template, so follow its own `template:` if it names one.
- A template is often a slide library: bios, case studies, the slides every
  pitch opens with, grouped into sections. `hype template slides <template>`
  lists them; `hype template slides <template> <n>` prints one, ready to
  paste. Copy those slides as they are rather than rewriting their content,
  and keep each case study's slides together.
- A template's `<Define>` components (sections, profiles, case studies) are
  the house style; prefer them to building the same layout by hand. A layout
  you repeat becomes a `<Define>` on one of the presentation's slides, or in
  the template.
- Every command takes `--json`, exits 0 on success and 1 on failure, and writes
  errors to stderr. `hype help <command>` lists a command's options.
