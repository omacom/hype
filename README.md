# Hype

Simple presentations, written in Markdown. Big headlines, images, video, and code—with a visual editor to put everything in order.

Hype is a native app for Omarchy. Your presentation is a Markdown file with its media alongside it. Choose an installed Omarchy theme, pick a font, and export to PDF or PowerPoint.

## Install

Install Hype from the [Omarchy Package Repository (OPR)](https://github.com/omacom/omarchy-pkgs):

```sh
omarchy pkg add hype
```

Then open **Hype** from the app launcher, or run `hype` in a terminal.

## Make a presentation

Open Hype from your app launcher. It reopens your last presentation; use **Ctrl+N** to start a new one, or **Ctrl+O** to choose a Markdown file.

In **Visual** mode, select a slide in the sidebar and write its Markdown below the preview. Changes appear as you type. Drag the divider to give the preview or editor more room. The mode button shows the current mode as a grid, slide, or `#` icon; click it to step through **Overview**, **Visual**, and **Markdown**. **Ctrl+M** flips the overview on and off, returning to the mode you came from, and **Ctrl+.** flips the Markdown source. Markdown mode edits the whole presentation, with the same formatting bar on top.

**Overview** mode fills the window with a grid of every slide. Select, rearrange, duplicate, and delete slides there as you would in the sidebar; the up and down arrows move by a row. Double-click a slide or press **Enter** to open it in Visual mode.

The buttons above the editor apply bold, italic, underline, headlines, code blocks, and hidden comments to your selection. Hype reads `*asterisks*` as italic and `_underscores_` as underline. **Media** adds an image or video; **Layout** opens its layout and background options.

Use **Ctrl+Enter** or right-click a slide to add a slide after the selection. Drag slides to rearrange them—their Markdown moves with them. Hold a dragged slide near the top or bottom of the sidebar or overview to scroll further. Select several slides with **Shift+click** or **Shift+arrows** to move, duplicate, or delete them together.

Hype saves automatically after a one-second typing pause, or every five seconds while you keep typing. Use **Ctrl+S** to choose a file for a new presentation or save immediately. Hype remembers the last directory you opened or saved to.

Every saved version stays in `.hype-backups/` beside your presentation. Hype also keeps local recovery snapshots, including unfinished Markdown and unnamed presentations, and restores your latest draft when you reopen after a crash. Choose **Version history** from the file menu in the top bar to restore an earlier snapshot; your current version remains available there too. Recovery snapshots contain Markdown and slide boundaries, not copies of images or videos. Unfinished code fences are backed up without replacing the last valid presentation file. Finish them before exporting; Hype checks that every slide will be preserved.

Local recovery snapshots live in `~/.local/state/hype/recovery/` (under `$XDG_STATE_HOME` if set). You can also recover a `.hype-backups/` version manually: copy its `.bak` file to a new `.md` file and open it.

## Write your slides

Separate slides with `---`, with a blank line on either side:

````markdown
# A big idea

---

# Keep it simple

- Write in Markdown
- Put your slides in order
- Tell your story

---

> Make something wonderful.

— Your closing thought

---

# Show the code

```ruby
class Presentation
  def next_slide
    slides.next
  end
end
```
````

Headlines are big by default. Quotes, lists, tables, and inline `code` work too. Ordinary line breaks stay visible on the slide. Code blocks fit the slide and use syntax highlighting when you specify a language, such as `ruby`, `rust`, `javascript`, `bash`, or `json`.

The single-slide editor hides the blank lines around slide separators, leaving just your content to edit.

## Add images and video

Paste an image or a copied image/video file with **Ctrl+V**. Hype asks for a name, saves the file, and adds it to the selected slide. Pasting onto a slide that already has media replaces that media while keeping the text. You can also drag a file onto the preview or use **+ Image / video** to replace the media. Dropping several files puts each additional file on a new slide.

Pasted still images are sized for a 4K slide without upscaling. Fitted images stay within 3840 × 2160; spanning images retain enough resolution to fill that area without discarding the parts outside the crop. Hype chooses a lossless PNG or WebP, keeping an existing file when it is already smaller and needs no resizing. Original files, videos, animated images, and SVGs are left intact.

Compression runs in the background. If it takes longer than a second, a progress bar appears over the slide; you can cancel the paste or wait for the filename prompt.

Media lives beside the Markdown file:

```text
my-talk/
  presentation.md
  images/
    city.jpg
    diagram.png
  videos/
    demo.mp4
```

Use just the filename; Hype finds the right directory:

```markdown
![](diagram.png)

---

![](city.jpg)

# A headline over a background

---

![](demo.mp4)
```

A lone image fits without cropping. Text on an image slide is always overlaid, with white lettering, subtle darkening, and a very light blur of the picture for readability. The text stays sharp, and pictures without text stay unblurred. An image with a headline spans by default; `fit` or `background=blur` keeps the whole image visible beneath the text. Videos fit the slide and play once when you reach them during a presentation.

Choose **Fit** or **Span** from the **Layout** menu above the editor, or put layout options inside the brackets:

| Markdown | Result |
| --- | --- |
| `![fit](photo.jpg)` | Show the whole image, with text overlaid |
| `![span](photo.jpg)` | Fill the slide, cropping as needed |
| `![loop muted](demo.mp4)` | Loop a video without sound |
| `![autoplay=false](demo.mp4)` | Wait for Space to play the video |

For images that leave space around them, Hype matches the background to the image’s edge color when possible. Choose **White**, **Black**, or **Use theme color** from the **Layout** menu to override it, or specify any color: `![fit background=#ffffff](diagram.png)`. Choosing White or Black also switches spanning media to fit so the background is visible.

Choose **Background → Blurred image** to fill the slide with a stretched, blurred copy behind the sharp fitted image: `![fit background=blur](portrait.jpg)`. The same background appears in PDF and PowerPoint exports. Animated images use their first frame for the blurred background.

For videos, choose **Background → Blurred first frame**, or write `![fit background=blur](portrait.mp4)`. The video plays over a still blur of its first frame, even when you specify a different poster image. Choosing blur also switches spanning media to fit so the background is visible.

**Background → Match image edges** also works with videos: `![fit background=auto](portrait.mp4)`. It samples the edges of the first frame and keeps that background color during playback, even with a custom poster. Selecting it switches spanning videos to fit.

Animated WebP and GIF images play inline in the preview and while presenting. Use the usual image syntax, such as `![](demo.webp)`, with the file in `images/`. Space pauses or resumes animations while presenting; PowerPoint exports automatically convert them to embedded MP4 videos, preserving the slide layout and playback settings. PDF exports capture their first frame.

Each slide supports one image or video. Copy the whole presentation folder when sharing or moving it.

## Choose your look

The palette and font icons in the toolbar choose an installed Omarchy theme and a presentation font. Hover to see the current choices. Theme colors apply to text, code, and slide backgrounds; your images keep their original colors. Code stays monospaced. Hype’s interface follows your current desktop theme independently and updates when you change it.

The header shows the presentation's name with your position in it, such as “Slide 4 of 45”; saving and exporting report their progress on that line. The file icon beside it holds New, Open, Save, Export, and Version history, and is highlighted when you have unsaved changes.

Colors and the font choice are saved in the Markdown file. Install the same font on another computer to keep the typography consistent.

## Present and export

Click **Present** or press **Ctrl+Space** (or **F5**) to toggle fullscreen presentation. Use the arrows to navigate, Space to play or pause video, and Escape to return to editing.

Finished videos hold their last frame. Press Space again to replay from the beginning.

Choose **Export as PDF** or **Export as PowerPoint** from the file menu in the top bar, or press **Ctrl+E** for PDF and **Ctrl+Shift+E** for PowerPoint, to share your presentation. Both exports are built into Hype. PowerPoint renders slides and converted animations at 4K (3840 × 2160). Slides preserve the rendered appearance rather than exposing editable text and shapes; the receiving computer does not need your fonts installed. Videos are embedded, and animated WebP/GIF images are converted to MP4 automatically without changing the original files. PDF captures still slides.

Export runs in the background. The top bar shows progress through rendering, video conversion, and packaging under the presentation name, with a **Cancel export** button. You can keep editing; the export uses the presentation as it was when you started. Failed or cancelled exports leave an existing file intact.

PDF keeps text as vectors and sizes embedded images for their visible area at 4K, omitting unused pixels outside spanning crops. Images use lossless compression to preserve fine detail. Photo-heavy PDFs can be larger than JPEG-compressed exports because they avoid additional compression artifacts.

PowerPoint export automatically converts other video formats, including WebM, to H.264 MP4 with AAC audio, leaving your originals untouched. Compatible MP4s are embedded directly. Use `fit` for videos that aren’t 16:9. Video autoplay and looping may vary between presentation apps; playback in Microsoft PowerPoint has not yet been verified.

## Use Hype from the command line

Hype's commands need no display, so a script or an AI agent can build a presentation from start to finish. A presentation is just a Markdown file: write it with any tool, then check, preview, and export it with `hype`.

```sh
hype new talk/presentation.md --title "My talk" --theme tokyo-night
hype check talk/presentation.md                   # every problem, with its slide and line
hype slides talk/presentation.md                  # an outline: number, lines, headline, media
hype render talk/presentation.md --slide 3 -o slide.png
hype render talk/presentation.md -o slides/       # every slide, plus slides.json
hype export talk/presentation.md talk.pdf         # or talk.pptx
hype themes
```

`check` reports all problems at once, such as missing media, invalid layout options, and unfinished code fences, and warns when a slide holds so much text that it shrinks below a readable size. `render --slide` writes a PNG even for a slide with problems, showing them on a banner, so you can look at what went wrong. Add `--json` to any command for structured output, and `--width` to `render` for another size. Commands exit 0 on success and 1 on failure, with errors on stderr.

`hype` alone lists the commands, and `hype open` starts the editor. `hype help format` prints the whole slide format, from front matter to media options, in a form an agent can read once and work from. `hype help <command>` lists a command's options.

To teach your coding agents about Hype, run `hype skill install`. It copies a short skill to `~/.agents/skills/hype/`, where Codex finds it, and links it into `~/.claude/skills/` for Claude Code. The skill points the agent at `hype help format`, so it stays current as Hype is upgraded. `hype skill` prints it instead.

If the presentation is open in the editor, changes written to the file appear there right away, and the editor stays on the slide you were viewing, even when slides are added or removed before it. **Ctrl+Z** undoes such a change. The editor never replaces unsaved changes of its own.

## Keyboard shortcuts

Slide navigation and selection shortcuts apply when the sidebar or preview has focus. Inside the Markdown editor, arrows and Shift+arrows move the cursor and select text.

| Shortcut | Action |
| --- | --- |
| Ctrl+N / Ctrl+O | New presentation / open file |
| Ctrl+S / Ctrl+Shift+S | Save / save as |
| Ctrl+E / Ctrl+Shift+E | Export as PDF / PowerPoint |
| Ctrl+M | Overview on / off |
| Ctrl+. | Markdown source on / off |
| Ctrl+B / Ctrl+I / Ctrl+U | Bold / italic / underline |
| Ctrl+H / Ctrl+K / Ctrl+/ | Headline / code block / hidden comment |
| ? / F1 | Show all shortcuts (F1 also works while typing) |
| Tab / Shift+Tab | Switch between sidebar and Markdown input |
| Arrow keys | Previous / next slide; up and down move by a row in Overview |
| Enter | Open the selected slide from Overview |
| Page Up / Page Down | Jump five slides, or five rows in Overview |
| Home / End | First / last slide |
| Ctrl+Up or Ctrl+Left | Move selected slides earlier (Ctrl+Up by a row in Overview) |
| Ctrl+Down or Ctrl+Right | Move selected slides later (Ctrl+Down by a row in Overview) |
| Shift+arrows / Shift+click | Extend the slide selection |
| Ctrl+Enter | Add a slide |
| Ctrl+D | Duplicate selected slides |
| Delete | Delete selected slides |
| Ctrl+V | Paste text or add and name media |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| Ctrl+Space / F5 | Toggle presentation |
| Escape | Leave presentation |
| Space | Play / pause video while presenting |

The mouse wheel over the sidebar selects the previous or next slide. Home/End jumps to the first/last slide throughout Visual mode, including its input field. In full Markdown mode, Home/End moves within the current line. In either editor, Page Up/Down scrolls a page; Ctrl+Home/End goes to the start/end of the text.

## Run from source

To build Hype yourself, install a C++17 compiler, make, Qt 6.9 or newer, FFmpeg, and GNU source-highlight; see [the package definition](pkgbuild/PKGBUILD) for dependencies. Then:

```sh
./bin/build
./build/hype open examples/welcome.md
```

`bin/build` uses a Qt toolchain under `build/qt/` when present, then falls back to `qmake6` on your PATH. It checks for Qt 6.9 or newer before compiling. Set `HYPE_QMAKE=/path/to/qmake6` to select a different toolchain.

For a launcher entry that rebuilds this checkout when opened, run `./bin/install-dev` and choose **Hype (Development)**.
