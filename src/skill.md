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
matter, slide separators, image and video options, and Mermaid flowchart
diagrams, for this version of Hype.

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

## Working with the person

- If they have the presentation open in the Hype editor, your saved edits appear
  there at once and the editor stays on the slide they are viewing. `hype open
  <file>` opens the editor for them; it needs their desktop, so do not wait on it.
- Slides are for an audience: a headline, a few bullets, or one image each. Prefer
  more slides over fuller ones.
- Every command takes `--json`, exits 0 on success and 1 on failure, and writes
  errors to stderr. `hype help <command>` lists a command's options.
