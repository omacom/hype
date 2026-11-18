"""Drive an exported deck's transitions in a browser.

The page's HypeDeck hook resolves a slide's transition (its own directive, then
the deck default, then none) and rasterizes any slide into a 1920 × 1080 canvas
for the WebGL overlay. The overlay itself needs a WebGL context; a headless
browser without one skips that check instead of failing the suite.
"""
import base64
import html
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHROMIUM = shutil.which('chromium') or shutil.which('chromium-browser') or shutil.which('google-chrome')


def png_data_uri(rgb):
    """A 2 × 2 opaque picture of one color, as a data URI."""
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    raw = (b'\0' + bytes(rgb) * 2) * 2
    data = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw))
            + chunk(b'IEND', b''))
    return 'data:image/png;base64,' + base64.b64encode(data).decode()


# A folder-export deck: four Markdown slides, the last a spanning picture.
FOLDER_PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck transitions</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source">---
transition: "liquid"
transition-duration: 0.25
---

# Transition probes
A line of text for the rasterizer to paint.

---

<!-- hype: transition="glitch" -->
# Override

---

<!-- hype: transition="none" -->
# Silent

---

<!-- hype: transition="bogus" -->
![span](__RED__)</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script type="application/json" id="transitions">{"liquid":"vec4 transition(vec2 uv) { return texture2D(toTex, uv); }","glitch":"vec4 transition(vec2 uv) { return texture2D(toTex, uv); }"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      window.addEventListener("load", async () => {
        const results = []
        const add = (ok, name, detail) =>
          results.push((ok ? "PASS " : "FAIL ") + name + (detail === undefined ? "" : " (" + detail + ")"))
        const hook = window.HypeDeck
        const hooked = !!hook && typeof hook.resolveTransition === "function"
          && typeof hook.slideTexture === "function"
        add(hooked, "HypeDeck hook")
        if (hooked) {
          const resolved = index => hook.resolveTransition(index) || {}
          const first = resolved(0), override = resolved(1), silent = resolved(2)
          const bogus = resolved(3)
          add(first.name === "liquid", "resolver slide 0 name", first.name)
          add(Math.abs(first.duration - 0.25) < 1e-9, "resolver slide 0 duration", first.duration)
          add(override.name === "glitch", "resolver slide 1 name", override.name)
          add(Math.abs(override.duration - 0.25) < 1e-9, "resolver slide 1 duration", override.duration)
          add(silent.name === "none", "resolver slide 2 name", silent.name)
          add(Math.abs(silent.duration - 0.25) < 1e-9, "resolver slide 2 duration", silent.duration)
          add(bogus.name === "liquid", "unknown slide override falls back", bogus.name)

          const content = document.querySelector("section.slide .text .content")
          const fitted = content ? content.style.fontSize : ""
          const texture = async index => {
            try { return await hook.slideTexture(index) } catch { return null }
          }
          const canvas = await texture(0)
          add(canvas instanceof HTMLCanvasElement, "slideTexture 0 canvas")
          if (canvas instanceof HTMLCanvasElement) {
            add(canvas.width === 1920 && canvas.height === 1080, "slideTexture 0 size",
                canvas.width + "x" + canvas.height)
            const data = canvas.getContext("2d").getImageData(0, 0, 1920, 1080).data
            const at = (x, y) => {
              const offset = (y * 1920 + x) * 4
              return [data[offset], data[offset + 1], data[offset + 2], data[offset + 3]]
            }
            const near = (actual, wanted, tolerance) =>
              actual.every((value, index) => Math.abs(value - wanted[index]) <= tolerance)
            const corner = at(4, 4)
            add(near(corner, [26, 27, 38, 255], 4), "rasterized background", corner.join(","))
            // Any ink at all proves the fitted DOM text reached the canvas.
            let ink = 0
            for (let offset = 0; offset < data.length; offset += 16) {
              const red = data[offset], green = data[offset + 1], blue = data[offset + 2]
              if (Math.abs(red - 26) + Math.abs(green - 27) + Math.abs(blue - 38) > 60) ink++
            }
            add(ink > 50, "rasterized text pixels", ink)
            add(!!fitted && content.style.fontSize === fitted, "fit unchanged by rasterizer",
                fitted + " -> " + (content ? content.style.fontSize : "missing"))
          }
          add(!!canvas && (await texture(0)) === canvas, "slideTexture cached")
          const picture = await texture(3)
          add(picture instanceof HTMLCanvasElement, "slideTexture 3 canvas")
          if (picture instanceof HTMLCanvasElement) {
            add(picture.width === 1920 && picture.height === 1080, "slideTexture 3 size",
                picture.width + "x" + picture.height)
            const data = picture.getContext("2d").getImageData(0, 0, 1920, 1080).data
            const offset = (540 * 1920 + 960) * 4
            const centered = [data[offset], data[offset + 1], data[offset + 2]]
            const red = centered[0] > 240 && centered[1] < 15 && centered[2] < 15
            add(red, "rasterized media pixel", centered.join(","))
          }
        }
        const report = document.createElement("pre")
        report.id = "results"
        report.textContent = results.join("\\n")
        document.body.appendChild(report)
      })
    </script>
  </body>
</html>
"""

# An unknown transition name and an oversized duration: the hook must report
# "none" and clamp the seconds to the presenter-friendly range.
CLAMP_PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck transition clamp</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source">---
transition: "missing"
transition-duration: 9
---

# One</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script type="application/json" id="transitions">{"liquid":"vec4 transition(vec2 uv) { return texture2D(toTex, uv); }"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      window.addEventListener("load", () => {
        const results = []
        const add = (ok, name, detail) =>
          results.push((ok ? "PASS " : "FAIL ") + name + (detail === undefined ? "" : " (" + detail + ")"))
        const hook = window.HypeDeck
        const hooked = !!hook && typeof hook.resolveTransition === "function"
        add(hooked, "HypeDeck hook")
        if (hooked) {
          const resolved = hook.resolveTransition(0) || {}
          add(resolved.name === "none", "unknown transition is none", resolved.name)
          add(Math.abs(resolved.duration - 4) < 1e-9, "duration clamped to 4", resolved.duration)
        }
        const report = document.createElement("pre")
        report.id = "results"
        report.textContent = results.join("\\n")
        document.body.appendChild(report)
      })
    </script>
  </body>
</html>
"""

# A baked deck (the one-file export): the slides are pictures already, so the
# transition engine reads them into WebGL textures. A null context is a skip.
BAKED_PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck transition overlay</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
      <section class="slide active"><img src="__RED__" alt=""></section>
      <section class="slide"><img src="__BLUE__" alt=""></section>
    </div>
    <script type="text/markdown" id="source">---
transition: "crossfade"
transition-duration: 4
---

# One

---

# Two</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script type="application/json" id="transitions">{"crossfade":"vec4 transition(vec2 uv) { return mix(texture2D(fromTex, uv), texture2D(toTex, uv), progress); }"}</script>
    <script>
      // Installed before deck.js so every overlay frame is counted. A transition
      // that animates draws at least once; sampling canvas display at a fixed
      // time would race virtual time in headless browsers.
      window.__transitionTrace = { draws: 0 }
      for (const name of ["WebGLRenderingContext", "WebGL2RenderingContext"]) {
        const type = window[name]
        if (!type) continue
        for (const method of ["drawArrays", "drawElements"]) {
          const original = type.prototype[method]
          if (typeof original !== "function") continue
          type.prototype[method] = function (...args) {
            window.__transitionTrace.draws++
            return original.apply(this, args)
          }
        }
      }
    </script>
    <script src="__SCRIPT__"></script>
    <script>
      window.addEventListener("load", async () => {
        const results = []
        const add = (ok, name, detail) =>
          results.push((ok ? "PASS " : "FAIL ") + name + (detail === undefined ? "" : " (" + detail + ")"))
        const hook = window.HypeDeck
        const hooked = !!hook && typeof hook.resolveTransition === "function"
          && typeof hook.slideTexture === "function"
        add(hooked, "HypeDeck hook on baked deck")
        if (hooked) {
          const resolved = hook.resolveTransition(1) || {}
          add(resolved.name === "crossfade", "resolver baked slide name", resolved.name)
          add(Math.abs(resolved.duration - 4) < 1e-9, "resolver baked duration", resolved.duration)
          let texture = null
          try { texture = await hook.slideTexture(0) } catch {}
          add(texture instanceof HTMLCanvasElement, "baked slide texture canvas")
          if (texture instanceof HTMLCanvasElement) {
            add(texture.width === 1920 && texture.height === 1080, "baked slide texture size",
                texture.width + "x" + texture.height)
            const data = texture.getContext("2d").getImageData(0, 0, 1920, 1080).data
            const offset = (540 * 1920 + 960) * 4
            const centered = [data[offset], data[offset + 1], data[offset + 2]]
            add(centered[0] > 240 && centered[1] < 15 && centered[2] < 15,
                "baked texture pixel", centered.join(","))
          }
        }
        // Headless browsers may refuse a WebGL context. That is the environment,
        // not the deck, so the overlay check is skipped rather than failed.
        const probe = document.createElement("canvas")
        const gl = probe.getContext("webgl", { alpha: false, antialias: false })
        if (!gl) {
          results.push("SKIP webgl unavailable in headless")
        } else {
          try {
            show(1)
            await new Promise(resolve => setTimeout(resolve, 250))
            const overlay = document.querySelector("canvas.transition")
            add(!!overlay, "transition overlay created")
            if (overlay) {
              add(overlay.width === 1920 && overlay.height === 1080, "transition overlay size",
                  overlay.width + "x" + overlay.height)
            }
            const draws = window.__transitionTrace.draws
            if (draws > 0) add(true, "transition drew frames", draws)
            else if (!overlay || (!overlay.getContext("webgl") && !overlay.getContext("webgl2")))
              results.push("SKIP engine webgl context unavailable")
            else add(false, "transition drew frames", draws)
          } catch (error) {
            add(false, "transition run", String(error))
          }
        }
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
class DeckTransitionTests(unittest.TestCase):
    def render(self, page):
        with tempfile.TemporaryDirectory() as directory:
            document = Path(directory) / 'index.html'
            document.write_text(page
                                .replace('__STYLESHEET__', (ROOT / 'src/deck.css').as_uri())
                                .replace('__SCRIPT__', (ROOT / 'src/deck.js').as_uri()))
            result = subprocess.run([CHROMIUM, '--headless=new', '--disable-gpu', '--no-sandbox',
                                     '--virtual-time-budget=5000', '--dump-dom', document.as_uri()],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            match = re.search(r'<pre id="results">(.*?)</pre>', result.stdout, re.S)
            self.assertIsNotNone(match, result.stdout + result.stderr)
            return html.unescape(match.group(1))

    def test_resolver_and_rasterizer(self):
        report = self.render(FOLDER_PAGE.replace('__RED__', png_data_uri((255, 0, 0))))
        self.assertNotIn('FAIL', report, report)
        # Hook, six resolver checks, and the rasterizer's size, pixels, and fit.
        self.assertGreaterEqual(report.count('PASS'), 14, report)

    def test_unknown_transition_and_duration_clamp(self):
        report = self.render(CLAMP_PAGE)
        self.assertNotIn('FAIL', report, report)
        self.assertEqual(report.count('PASS'), 3, report)

    def test_overlay_runs_or_skips_without_webgl(self):
        report = self.render(BAKED_PAGE
                             .replace('__RED__', png_data_uri((255, 0, 0)))
                             .replace('__BLUE__', png_data_uri((0, 0, 255))))
        self.assertNotIn('FAIL', report, report)
        # The hook, resolver, and baked texture checks do not need WebGL.
        self.assertGreaterEqual(report.count('PASS'), 6, report)
        self.assertTrue('PASS transition drew frames' in report or 'SKIP webgl' in report
                        or 'SKIP engine webgl' in report, report)


if __name__ == '__main__':
    unittest.main()
