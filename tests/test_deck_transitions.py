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
![span](__RED__)

---

```html
<!-- hype: transition="glitch" -->
```
# Fenced example</script>
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
          const bogus = resolved(3), example = resolved(4)
          add(first.name === "liquid", "resolver slide 0 name", first.name)
          add(Math.abs(first.duration - 0.25) < 1e-9, "resolver slide 0 duration", first.duration)
          add(override.name === "glitch", "resolver slide 1 name", override.name)
          add(Math.abs(override.duration - 0.25) < 1e-9, "resolver slide 1 duration", override.duration)
          add(silent.name === "none", "resolver slide 2 name", silent.name)
          add(Math.abs(silent.duration - 0.25) < 1e-9, "resolver slide 2 duration", silent.duration)
          add(bogus.name === "liquid", "unknown slide override falls back", bogus.name)
          add(example.name === "liquid", "fenced directive is not an override", example.name)

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

# A deck whose first slide is an animated picture. Leaving it must repaint the
# cached texture, because the picture has moved past the frame it was painted
# with; a slide of text keeps its cache.
MOVING_PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck moving media</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source">---
transition: "crossfade"
transition-duration: 4
---

![](images/anim.gif)

---

# Still

---

# Third</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script type="application/json" id="transitions">{"crossfade":"vec4 transition(vec2 uv) { return mix(texture2D(fromTex, uv), texture2D(toTex, uv), progress); }"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      window.addEventListener("load", async () => {
        const results = []
        const add = (ok, name, detail) =>
          results.push((ok ? "PASS " : "FAIL ") + name + (detail === undefined ? "" : " (" + detail + ")"))
        const hook = window.HypeDeck
        const hooked = !!hook && typeof hook.slideTexture === "function"
        add(hooked, "HypeDeck hook")
        if (hooked) {
          // Every in-place repaint goes through this wrapper, so the test can
          // tell a fresh paint from an upload of the cache.
          const repaints = []
          const paint = window.paintSlideToCanvas
          add(typeof paint === "function", "rasterizer instrumentable")
          window.paintSlideToCanvas = function (section, output, ...rest) {
            if (output) repaints.push({ section, output })
            return paint(section, output, ...rest)
          }
          const sections = [...document.querySelectorAll("section.slide")]
          const texture = hook.slideTexture(0)
          add(texture instanceof HTMLCanvasElement, "moving texture canvas")
          add(!!hook.slideTexture(1), "static texture canvas")

          const direct = repaints.length
          hook.slideTexture(0, true)
          add(repaints.length === direct + 1 && repaints[direct].section === sections[0],
              "refresh repaints moving media", repaints.length - direct)
          add(hook.slideTexture(0) === texture, "refresh keeps texture identity")
          hook.slideTexture(1, true)
          add(repaints.length === direct + 1, "refresh keeps static cache", repaints.length - direct)

          // The transition engine is the only other caller; headless browsers
          // may refuse it a context, which is a skip rather than a failure.
          const probe = document.createElement("canvas")
          if (!probe.getContext("webgl", { alpha: false, antialias: false })) {
            results.push("SKIP webgl unavailable in headless")
          } else {
            const before = repaints.length
            window.show(1)
            const leaving = repaints.slice(before).filter(entry => entry.section === sections[0])
            add(leaving.length === 1, "departing moving texture repainted", leaving.length)
            add(leaving.length === 1 && leaving[0].output === texture,
                "repaint reused the cached canvas")

            window.show(0)
            const entering = repaints.slice(before).filter(entry => entry.section === sections[0])
            add(entering.length === 2, "incoming moving texture repainted", entering.length)

            const still = repaints.length
            window.show(2)
            add(repaints.slice(still).every(entry => entry.section !== sections[1]),
                "static texture stayed cached")
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


# A deck longer than the texture cache's limit. Visiting every slide must leave
# only the last few textures behind: a transition needs two, and the rest would
# hold pixel memory for slides the viewer has left.
BOUNDED_PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck texture cache</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source">---
transition: "crossfade"
transition-duration: 0.1
---

# One

---

# Two

---

# Three

---

# Four

---

# Five

---

# Six

---

# Seven

---

# Eight</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script type="application/json" id="transitions">{"crossfade":"vec4 transition(vec2 uv) { return mix(texture2D(fromTex, uv), texture2D(toTex, uv), progress); }"}</script>
    <script src="__SCRIPT__"></script>
    <script>
      window.addEventListener("load", () => {
        const results = []
        const add = (ok, name, detail) =>
          results.push((ok ? "PASS " : "FAIL ") + name + (detail === undefined ? "" : " (" + detail + ")"))
        const hook = window.HypeDeck
        const counting = !!hook && typeof hook.slideTexture === "function"
          && typeof hook.textureCount === "number"
        add(counting, "HypeDeck textureCount hook", counting ? hook.textureCount : "missing")
        if (counting) {
          // The deck's own limit: a transition holds two textures at once.
          const limit = 4
          const sections = [...document.querySelectorAll("section.slide")]
          // Visiting asks for textures: leaving a slide and arriving at the next
          // is two requests. The overlay may be refused a WebGL context here, so
          // the cache's entry point is then driven the way a transition drives it.
          for (let index = 1; index < sections.length; index++) {
            try { window.show(index) } catch {}
          }
          add(hook.textureCount <= limit, "visiting stays within the limit", hook.textureCount)
          const textures = []
          for (let index = 0; index < sections.length; index++)
            textures.push(hook.slideTexture(index))
          add(textures.every(texture => texture instanceof HTMLCanvasElement),
              "every slide rasterizes")
          add(hook.textureCount <= limit, "texture cache bounded", hook.textureCount)
          add(hook.textureCount < sections.length, "cache smaller than the deck",
              hook.textureCount + " of " + sections.length)

          const size = hook.textureCount
          add(hook.slideTexture(5) === textures[5], "cached slide keeps its texture")
          add(hook.textureCount === size, "re-reading a cached slide does not grow the cache")
          const repainted = hook.slideTexture(0)
          add(repainted instanceof HTMLCanvasElement && repainted !== textures[0],
              "evicted slide repaints on demand")
          add(hook.textureCount <= limit, "repaint stays within the limit", hook.textureCount)
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


# A picture that arrives after the transition asked for it. The overlay must not
# upload an empty canvas: the transition waits for the repaint, or skips.
READINESS_PAGE = """<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <title>deck media readiness</title>
    <link rel="stylesheet" href="__STYLESHEET__">
  </head>
  <body>
    <div class="stage" id="stage">
      <button type="button" class="nav prev" aria-label="Previous">&#8249;</button>
      <button type="button" class="nav next" aria-label="Next">&#8250;</button>
      <div class="counter" id="counter"></div>
    </div>
    <script type="text/markdown" id="source">---
transition: "crossfade"
transition-duration: 0.2
---

# First

---

![late](__RED__)

---

![later](__BLUE__)</script>
    <script type="application/json" id="palette">{"background": "#1a1b26", "foreground": "#a9b1d6", "accent": "#7aa2f7"}</script>
    <script type="application/json" id="transitions">{"crossfade":"vec4 transition(vec2 uv) { return mix(texture2D(fromTex, uv), texture2D(toTex, uv), progress); }"}</script>
    <script>
      // Installed before deck.js so uploads and frames are counted from the
      // first transition. The six-argument texImage2D form uploads a picture;
      // the nine-argument form allocates the texture.
      window.__transitionTrace = { draws: 0, uploads: 0 }
      for (const name of ["WebGLRenderingContext", "WebGL2RenderingContext"]) {
        const type = window[name]
        if (!type) continue
        const draw = type.prototype.drawArrays
        if (typeof draw === "function")
          type.prototype.drawArrays = function (...args) {
            window.__transitionTrace.draws++
            return draw.apply(this, args)
          }
        const upload = type.prototype.texImage2D
        if (typeof upload === "function")
          type.prototype.texImage2D = function (...args) {
            if (args.length === 6) window.__transitionTrace.uploads++
            return upload.apply(this, args)
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
        const hooked = !!hook && typeof hook.slideReady === "function"
        add(hooked, "HypeDeck readiness hook")
        if (hooked) {
          const sections = [...document.querySelectorAll("section.slide")]
          const first = sections[1].querySelector("img.media")
          const second = sections[2].querySelector("img.media")
          const red = first && first.getAttribute("src")
          const blue = second && second.getAttribute("src")
          add(!!first && !!second, "picture slides rendered")
          const trace = window.__transitionTrace
          const slideActive = index => sections[index].classList.contains("active")
          const webgl = !!document.createElement("canvas")
            .getContext("webgl", { alpha: false, antialias: false })
          const loaded = image => new Promise(resolve => {
            if (image.complete && image.naturalWidth) resolve()
            else image.addEventListener("load", resolve, { once: true })
          })

          // A picture with no source has no decoded frame, so the slide is not
          // ready and a transition into it must not upload or draw.
          first.removeAttribute("src")
          add(hook.slideReady(0) === true, "text slide ready")
          add(hook.slideReady(1) === false, "undecoded slide not ready")
          const uploads = trace.uploads, draws = trace.draws
          window.show(1)
          // Its picture is missing, so the overlay is still preparing: the
          // outgoing slide stays on screen instead of the new one flashing.
          if (webgl) add(!slideActive(1) && slideActive(0), "outgoing slide held while preparing")
          else results.push("SKIP webgl unavailable in headless")
          await new Promise(resolve => setTimeout(resolve, 450))
          add(slideActive(1) && !slideActive(0), "new slide shown when the wait ends")
          add(trace.uploads === uploads, "no upload without ready media", trace.uploads - uploads)
          add(trace.draws === draws, "no animation without ready media", trace.draws - draws)
          const overlay = document.querySelector("canvas.transition")
          add(!overlay || overlay.style.display === "none", "overlay stayed hidden")

          // The data URI arrives after the transition gave up. The slide is
          // ready now, but the skipped animation stays skipped.
          first.src = red
          await loaded(first)
          add(hook.slideReady(1) === true, "late picture becomes ready")

          // A picture that arrives while the transition is still waiting is
          // painted and uploaded, so the deferred animation runs.
          second.removeAttribute("src")
          const deferred = trace.draws
          window.show(2)
          add(hook.slideReady(2) === false, "incoming slide not ready")
          if (webgl) add(!slideActive(2) && slideActive(1), "second slide held while preparing")
          second.src = blue
          await loaded(second)
          await new Promise(resolve => setTimeout(resolve, 250))
          add(slideActive(2) && !slideActive(1), "new slide shown when the transition starts")
          if (trace.draws > deferred) add(true, "deferred transition drew frames", trace.draws - deferred)
          else if (!document.querySelector("canvas.transition")) results.push("SKIP webgl unavailable in headless")
          else add(false, "deferred transition drew frames", trace.draws - deferred)
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
    def render(self, page, files=None):
        with tempfile.TemporaryDirectory() as directory:
            for name, content in (files or {}).items():
                target = Path(directory) / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
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

    def test_moving_media_texture_repaints_at_transition(self):
        animated = (ROOT / 'tests/fixtures/animated.gif').read_bytes()
        report = self.render(MOVING_PAGE, {'images/anim.gif': animated})
        self.assertNotIn('FAIL', report, report)
        # Hook, instrumentation, both textures, and the direct refresh checks.
        self.assertGreaterEqual(report.count('PASS'), 7, report)
        self.assertIn('PASS refresh repaints moving media', report)
        self.assertTrue('PASS departing moving texture repainted' in report
                        or 'SKIP webgl' in report, report)

    def test_texture_cache_stays_bounded(self):
        report = self.render(BOUNDED_PAGE)
        self.assertNotIn('FAIL', report, report)
        self.assertIn('PASS every slide rasterizes', report)
        self.assertIn('PASS texture cache bounded', report)
        self.assertIn('PASS cache smaller than the deck', report)
        self.assertIn('PASS evicted slide repaints on demand', report)
        self.assertIn('PASS repaint stays within the limit', report)

    def test_incomplete_media_waits_for_repaint(self):
        page = (READINESS_PAGE
                .replace('__RED__', png_data_uri((255, 0, 0)))
                .replace('__BLUE__', png_data_uri((0, 0, 255))))
        report = self.render(page)
        self.assertNotIn('FAIL', report, report)
        self.assertIn('PASS undecoded slide not ready', report)
        self.assertTrue('PASS outgoing slide held while preparing' in report
                        or 'SKIP webgl' in report, report)
        self.assertIn('PASS new slide shown when the wait ends', report)
        self.assertIn('PASS no upload without ready media', report)
        self.assertIn('PASS no animation without ready media', report)
        self.assertIn('PASS late picture becomes ready', report)
        self.assertIn('PASS incoming slide not ready', report)
        self.assertTrue('PASS second slide held while preparing' in report
                        or 'SKIP webgl' in report, report)
        self.assertIn('PASS new slide shown when the transition starts', report)
        self.assertTrue('PASS deferred transition drew frames' in report or 'SKIP webgl' in report,
                        report)


if __name__ == '__main__':
    unittest.main()
