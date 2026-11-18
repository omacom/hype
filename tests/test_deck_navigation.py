"""Drive the exported deck's navigation in a browser: a swipe moves one slide."""
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
    <title>deck navigation</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source"># One
---
# Two
---
# Three</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      (() => {
        const stage = document.getElementById("stage")
        const counter = document.getElementById("counter")
        const results = []
        function gesture(target, from, to) {
          const init = { bubbles: true, cancelable: true, clientX: from, clientY: 100,
                         pointerId: 1, pointerType: "touch", isPrimary: true }
          target.dispatchEvent(new PointerEvent("pointerdown", init))
          target.dispatchEvent(new PointerEvent("pointerup", { ...init, clientX: to }))
          // A browser sends a click after the pointerup; a swipe must not use it.
          target.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true,
                                                         clientX: to, clientY: 100 }))
        }
        function check(name, start, target, from, to, want) {
          show(start)
          gesture(target, from, to)
          const got = counter.textContent
          results.push((got === want ? "PASS " : "FAIL ") + name + ": " + got + " (want " + want + ")")
        }
        const next = stage.querySelector(".next")
        check("left swipe", 0, stage, 400, 300, "2 / 3")
        check("right swipe", 1, stage, 300, 400, "1 / 3")
        check("tap", 0, stage, 400, 400, "2 / 3")
        check("left swipe on the next button", 0, next, 400, 300, "2 / 3")
        const report = document.createElement("pre")
        report.id = "results"
        report.textContent = results.join("\\n")
        document.body.appendChild(report)
      })()
    </script>
  </body>
</html>
"""


def page(stylesheet, script):
    return PAGE.replace('__STYLESHEET__', stylesheet).replace('__SCRIPT__', script)


@unittest.skipUnless(CHROMIUM, 'chromium is not installed')
class DeckNavigationTests(unittest.TestCase):
    def test_swipe_and_tap_each_move_one_slide(self):
        with tempfile.TemporaryDirectory() as directory:
            document = Path(directory) / 'index.html'
            document.write_text(page((ROOT / 'src/deck.css').as_uri(), (ROOT / 'src/deck.js').as_uri()))
            result = subprocess.run([CHROMIUM, '--headless=new', '--disable-gpu', '--no-sandbox',
                                     '--dump-dom', document.as_uri()],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            match = re.search(r'<pre id="results">(.*?)</pre>', result.stdout, re.S)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            report = html.unescape(match.group(1))
            self.assertNotIn('FAIL', report, report)
            self.assertEqual(report.count('PASS'), 4, report)


if __name__ == '__main__':
    unittest.main()
