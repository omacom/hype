"""Draw an exported deck's svg blocks in a browser: the drawing appears, but it
is an image rather than page markup, so a shared deck cannot run script or
restyle the page from inside a drawing."""
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
    <title>deck svg</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source"># Hostile drawing

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 120" onload="document.title='pwned'">
  <style>html, body { background: rgb(255, 0, 0) !important }</style>
  <rect width="300" height="120" fill="var(--accent)"/>
</svg>
```

---
# Ordinary drawing

```svg
<svg viewBox="0 0 300 120">
  <rect x="120" y="20" width="60" height="100" fill="var(--accent)"/>
</svg>
```</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7", "font": "Acme & Co \\\"Sharp\\\" <Bold>"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      window.addEventListener("load", () => {
        const results = []
        const boxes = document.querySelectorAll(".svg-box")
        const images = document.querySelectorAll(".svg-box img")
        results.push((boxes.length === 2 && images.length === 2 ? "PASS " : "FAIL ")
          + "drawings are images: boxes=" + boxes.length + " images=" + images.length)
        // An inserted drawing would be live markup in the deck's own document.
        const markup = document.querySelectorAll(".svg-box svg, .svg-box script").length
        results.push((markup === 0 ? "PASS " : "FAIL ") + "drawing markup in page: " + markup)
        // Neither an svg onload handler nor an svg <style> may reach the deck.
        results.push((document.title === "deck svg" ? "PASS " : "FAIL ")
          + "title: " + document.title)
        const background = getComputedStyle(document.body).backgroundColor
        results.push((background !== "rgb(255, 0, 0)" ? "PASS " : "FAIL ")
          + "body background: " + background)
        // The ordinary drawing still renders, at its viewBox size.
        const ordinary = images[1]
        const drawn = ordinary && ordinary.naturalWidth === 300 && ordinary.naturalHeight === 120
        results.push((drawn ? "PASS " : "FAIL ") + "ordinary drawing: "
          + (ordinary ? ordinary.naturalWidth + "x" + ordinary.naturalHeight : "missing"))
        // A font with XML metacharacters must still leave every drawing decodable.
        const decoded = Array.from(images).every(image => image.complete && image.naturalWidth > 0)
        const broken = Array.from(images).filter(image => !image.complete || image.naturalWidth === 0)
          .map(image => image.src.slice(0, 40)).join(", ")
        results.push((decoded ? "PASS " : "FAIL ")
          + "drawings decode with hostile font: " + (decoded ? "yes" : broken))
        const report = document.createElement("pre")
        report.id = "results"
        report.textContent = results.join("\\n")
        document.body.appendChild(report)
      })
    </script>
  </body>
</html>
"""


@unittest.skipUnless(CHROMIUM, 'chromium is not installed')
class DeckSvgTests(unittest.TestCase):
    def test_svg_blocks_render_as_images_not_markup(self):
        with tempfile.TemporaryDirectory() as directory:
            document = Path(directory) / 'index.html'
            document.write_text(PAGE
                                .replace('__STYLESHEET__', (ROOT / 'src/deck.css').as_uri())
                                .replace('__SCRIPT__', (ROOT / 'src/deck.js').as_uri()))
            result = subprocess.run([CHROMIUM, '--headless=new', '--disable-gpu', '--no-sandbox',
                                     '--virtual-time-budget=3000', '--dump-dom', document.as_uri()],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            match = re.search(r'<pre id="results">(.*?)</pre>', result.stdout, re.S)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            report = html.unescape(match.group(1))
            self.assertNotIn('FAIL', report, report)
            self.assertEqual(report.count('PASS'), 6, report)


if __name__ == '__main__':
    unittest.main()
