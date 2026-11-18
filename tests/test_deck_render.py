"""Render the exported deck's slides in a browser: fitted text stays inside the
slide, and lists render as lists. Both were broken when slides were fitted while
still display: none, which measures zero and lets every font size "fit"."""
import html
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHROMIUM = shutil.which('chromium') or shutil.which('chromium-browser') or shutil.which('google-chrome')

PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck rendering</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source"># Rendering

---
<!-- A long block of prose, oversized unless the fit measures the hidden slide -->
Line one of a long block of prose that wraps around a bit.
Line two of a long block of prose that wraps around a bit.
Line three of a long block of prose that wraps around a bit.
Line four of a long block of prose that wraps around a bit.
Line five of a long block of prose that wraps around a bit.
Line six of a long block of prose that wraps around a bit.
Line seven of a long block of prose that wraps around a bit.
Line eight of a long block of prose that wraps around a bit.
Line nine of a long block of prose that wraps around a bit.
Line ten of a long block of prose that wraps around a bit.
Line eleven of a long block of prose that wraps around a bit.
Line twelve of a long block of prose that wraps around a bit.
Line thirteen of a long block of prose that wraps around a bit.
Line fourteen of a long block of prose that wraps around a bit.
Line fifteen of a long block of prose that wraps around a bit.
Line sixteen of a long block of prose that wraps around a bit.
Line seventeen of a long block of prose that wraps around a bit.
Line eighteen of a long block of prose that wraps around a bit.
Line nineteen of a long block of prose that wraps around a bit.
Line twenty of a long block of prose that wraps around a bit.

---
- First bullet point with a longer line of text that will wrap
- Second bullet point with a longer line of text that will wrap
- Third bullet point with a longer line of text that will wrap

---
1. First numbered step with a longer line of text that will wrap
2. Second numbered step with a longer line of text that will wrap
3. Third numbered step with a longer line of text that will wrap

---
A pipe is an ordinary character | not proof of a table.

| Name | Value |
| --- | --- |
| alpha | 1 |

---
![overlay=0.8](missing.png) An explicit overlay still darkens the picture.

---
![overlay=0](missing.png) Choosing no overlay turns the darkening off.

---
# Blurred backdrop
![background=blur](missing.png)

---
# Spanning backdrop
![span background=blur](missing.png)

---
First line of a wrapped paragraph.
Second line of a wrapped paragraph.
Third line with an explicit break marker\\

---
Use `hype \\
now` to run it.</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      (() => {
        const results = []
        const area = { width: 1660, height: 900 }
        const wantedKind = { 2: "UL", 3: "OL" }
        for (const [index, section] of document.querySelectorAll("section.slide").entries()) {
          const label = "slide " + index
          const content = section.querySelector(".text .content")
          if (!content) { results.push("FAIL " + label + ": no text rendered"); continue }
          // Hidden slides measure zero, as the fit itself learned the hard way.
          section.classList.add("measuring")
          const width = content.scrollWidth, height = content.offsetHeight
          const fits = height <= area.height + 1 && width <= area.width + 1
          const font = parseFloat(getComputedStyle(content).fontSize)
          section.classList.remove("measuring")
          results.push((fits ? "PASS " : "FAIL ") + label + " fits: font=" + font
            + " content=" + width + "x" + height)
          // Twenty lines must shrink well below the 128px ceiling a broken fit leaves.
          if (index === 1 && font >= 100)
            results.push("FAIL " + label + " oversized at " + font + "px")
          if (index in wantedKind) {
            const list = section.querySelector("ul, ol")
            const kind = list ? list.tagName : "none"
            const items = list ? list.children.length : 0
            const right = kind === wantedKind[index] && items === 3
            results.push((right ? "PASS " : "FAIL ") + label + " list " + kind
              + " items=" + items)
          }
          if (index === 4) {
            // A prose line with a pipe stays prose; the table below it still renders.
            const paragraphs = content.querySelectorAll("p").length
            const tables = content.querySelectorAll("table")
            const rows = tables.length ? tables[0].querySelectorAll("tbody tr").length : 0
            const right = paragraphs === 1 && tables.length === 1 && rows === 1
            results.push((right ? "PASS " : "FAIL ") + label + " pipe prose p=" + paragraphs
              + " tables=" + tables.length + " rows=" + rows)
          }
          if (index === 5 || index === 6) {
            // overlay= replaces the default darkening, including turning it off.
            const layer = section.querySelector(".overlay")
            const color = layer ? getComputedStyle(layer).backgroundColor : "none"
            const expected = index === 5 ? "rgba(0, 0, 0, 0.8)" : "none"
            results.push((color === expected ? "PASS " : "FAIL ") + label + " overlay " + color)
          }
          if (index === 7 || index === 8) {
            // A headline no longer keeps background=blur spanning: the picture fits
            // and a blurred copy fills the margins. An explicit span still spans.
            const backdrop = section.querySelector(".backdrop")
            const covered = section.querySelector(".media.cover")
            const right = index === 7 ? (!!backdrop && !covered) : (!backdrop && !!covered)
            results.push((right ? "PASS " : "FAIL ") + label + " blur backdrop="
              + !!backdrop + " span=" + !!covered)
          }
          if (index === 9) {
            // Ordinary line breaks stay visible: each becomes a <br>, and an explicit
            // hard-break marker is not left behind as text.
            const paragraph = content.querySelector("p")
            const breaks = paragraph ? paragraph.querySelectorAll("br").length : 0
            const text = paragraph ? paragraph.textContent : ""
            const right = breaks === 2 && text.startsWith("First line")
              && text.includes("Third line") && !text.includes("\\\\")
            results.push((right ? "PASS " : "FAIL ") + label + " line breaks=" + breaks
              + " text=" + text)
          }
          if (index === 10) {
            // A backslash inside a code span that crosses a line boundary is code,
            // not a hard-break marker, so it must survive to the slide.
            const code = content.querySelector("p code")
            const text = code ? code.textContent : ""
            const right = text.includes("hype") && text.includes("now")
              && text.includes("\\\\")
            results.push((right ? "PASS " : "FAIL ") + label + " code backslash=" + text)
          }
        }
        const wanted = document.querySelectorAll("section.slide").length
        const rendered = document.querySelectorAll("section.slide .text").length
        results.push((rendered === wanted ? "PASS " : "FAIL ")
          + "slides with text: " + rendered + " of " + wanted)
        const report = document.createElement("pre")
        report.id = "results"
        report.textContent = results.join("\\n")
        document.body.appendChild(report)
      })()
    </script>
  </body>
</html>
"""


@unittest.skipUnless(CHROMIUM, 'chromium is not installed')
class DeckRenderTests(unittest.TestCase):
    def test_fitted_text_and_lists_render(self):
        with tempfile.TemporaryDirectory() as directory:
            document = Path(directory) / 'index.html'
            document.write_text(PAGE
                                .replace('__STYLESHEET__', (ROOT / 'src/deck.css').as_uri())
                                .replace('__SCRIPT__', (ROOT / 'src/deck.js').as_uri()))
            result = subprocess.run([CHROMIUM, '--headless=new', '--disable-gpu', '--no-sandbox',
                                     '--virtual-time-budget=2000', '--dump-dom', document.as_uri()],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            match = re.search(r'<pre id="results">(.*?)</pre>', result.stdout, re.S)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            report = html.unescape(match.group(1))
            self.assertNotIn('FAIL', report, report)
            # Every slide fits and is measured: one fit line per slide, plus the
            # list, table, overlay, blur, line-break, code-backslash, and
            # rendered-slide checks.
            self.assertEqual(report.count('PASS'), 21, report)


if __name__ == '__main__':
    unittest.main()
