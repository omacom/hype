"use strict"

// Navigation is shared by every deck: baked decks ship their slides as
// <section> markup, while an editable folder export ships its Markdown in a
// <script type="text/markdown"> block, rendered below into the same shape.

const stage = document.getElementById("stage")
const counter = document.getElementById("counter")
const prev = stage.querySelector(".prev")
const next = stage.querySelector(".next")

// ---------------------------------------------------------------------------
// The editable folder export: Hype's slide layout, rendered in the browser.
// Slides live in a 1920 × 1080 canvas scaled to the window, text is fitted by
// the same binary search the editor uses, and media follows the same
// directives. Edit the Markdown block in index.html and refresh.

const SLIDE_W = 1920, SLIDE_H = 1080
const FENCE = /^ {0,3}(`{3,}|~{3,})(.*)$/
const MEDIA_RE = /!\[([^\]]*)\]\((?:<([^>]+)>|([^\s)]+))\)/
const DIRECTIVE_RE = /([a-z]+)(?:=("(?:[^"\\]|\\.)*"|[^\s]+))?/g
const COLOR_RE = /^#[0-9a-fA-F]{6}$/
const VIDEO_EXT = ["mp4", "m4v", "mov", "webm", "mkv"]
const TEXT_AREA = { left: 130, top: 90, width: 1660, height: 900 }
const BAND_AREA = { left: 130, top: 40, width: 1660, height: 205 }
const FILL_AREA = { left: 70, top: 50, width: 1780, height: 980 }
const BELOW_AREA = { left: 100, top: 280, width: 1720, height: 730 }
const PLACEMENTS = {
  "top-left": ["left", "top"], top: ["center", "top"], "top-right": ["right", "top"],
  left: ["left", "center"], center: ["center", "center"], right: ["right", "center"],
  "bottom-left": ["left", "bottom"], bottom: ["center", "bottom"], "bottom-right": ["right", "bottom"],
}
// What each rendered slide needs to be drawn a second time, onto a canvas: the
// parsed media and palette, the text area, and the neutral block list. The DOM
// stays the fitting authority; the map only saves a reparse.
const slideStates = new WeakMap()

// --- Parsing ---------------------------------------------------------------

function parseDeck(source) {
  const result = { header: "", slides: [] }
  if (source.startsWith("﻿")) source = source.slice(1)
  const lines = source.split("\n").map(line => (line.endsWith("\r") ? line.slice(0, -1) : line))
  let start = 0
  if (lines[0] === "---") {
    let closed = -1
    for (let i = 1; i < lines.length; i++)
      if (lines[i] === "---") { closed = i; break }
    if (closed < 0) {
      result.slides = [source]
      return result
    }
    result.header = lines.slice(0, closed + 1).join("\n") + "\n"
    start = closed + 1
  }
  const inFence = lines.map(() => false)
  let fenceRun = null
  for (let i = start; i < lines.length; i++) {
    const match = FENCE.exec(lines[i])
    if (match) {
      const run = match[1]
      if (!fenceRun) fenceRun = run
      else if (run[0] === fenceRun[0] && run.length >= fenceRun.length && !match[2].trim())
        fenceRun = null
      inFence[i] = true
      continue
    }
    inFence[i] = fenceRun !== null
  }
  for (let i = start; i < lines.length; i++)
    if (!inFence[i] && lines[i] === "---") {
      result.slides.push(lines.slice(start, i).join("\n"))
      start = i + 1
    }
  result.slides.push(lines.slice(start).join("\n"))
  return result
}

function scalar(header, key) {
  const match = new RegExp("^" + key + ":\\s*([^\\r\\n]*)$", "m").exec(header)
  if (!match) return null
  let value = match[1].trim()
  if (value.startsWith('"') && value.endsWith('"')) {
    try {
      const parsed = JSON.parse("[" + value + "]")
      if (Array.isArray(parsed) && parsed.length) return String(parsed[0])
    } catch {}
  }
  if (value.startsWith("'") && value.endsWith("'")) return value.slice(1, -1).replace(/''/g, "'")
  return value
}

function outsideCode(source, maskInline = true) {
  const lines = source.split("\n")
  let fenceRun = null
  const masked = lines.map(line => {
    const match = FENCE.exec(line)
    let hidden = fenceRun !== null
    if (match) {
      const run = match[1]
      hidden = true
      if (!fenceRun) fenceRun = run
      else if (run[0] === fenceRun[0] && run.length >= fenceRun.length && !match[2].trim())
        fenceRun = null
    }
    return hidden || line.startsWith("    ") ? " ".repeat(line.length) : line
  })
  let result = masked.join("\n")
  if (!maskInline) return result
  return result.replace(/(`+)([^`]|`(?!`))*?\1/g, spaces => " ".repeat(spaces.length))
}

function withoutComments(source) {
  const masked = outsideCode(source)
  const ranges = []
  let match
  const comments = /<!--[\s\S]*?-->/g
  while ((match = comments.exec(masked))) ranges.push([match.index, match[0].length])
  for (const [start, length] of ranges.reverse())
    source = source.slice(0, start) + source.slice(start + length)
  return source
}

function slideProperty(source, key) {
  const match = new RegExp("<!--\\s*hype:[\\s\\S]*?\\b" + key + '="([^"]*)"').exec(outsideCode(source))
  return match ? match[1] : ""
}

function takeFence(text, language) {
  // Removes one ```language fence from the text, returning its body.
  const lines = text.split("\n")
  let fenceRun = null, start = -1
  for (let i = 0; i < lines.length; i++) {
    const match = FENCE.exec(lines[i])
    if (!match) continue
    const run = match[1]
    if (!fenceRun) {
      fenceRun = run
      if (match[2].trim().split(/\s+/)[0].toLowerCase() === language) start = i
    } else if (run[0] === fenceRun[0] && run.length >= fenceRun.length && !match[2].trim()) {
      if (start >= 0) {
        const body = lines.slice(start + 1, i).join("\n")
        lines.splice(start, i - start + 1)
        return { body, text: lines.join("\n") }
      }
      fenceRun = null
    }
  }
  return null
}

function isVideoFile(file) {
  const dot = file.lastIndexOf(".")
  return dot >= 0 && VIDEO_EXT.includes(file.slice(dot + 1).toLowerCase())
}

function assetPath(file, video) {
  if (/^[a-z]+:/i.test(file) || file.startsWith("/")) return file
  return file.startsWith("images/") || file.startsWith("videos/")
    ? file
    : (video ? "videos/" : "images/") + file
}

function parseMedia(source) {
  const media = {
    file: "", video: false, span: false, loop: false, muted: false,
    autoplay: true, overlay: 0, background: "", poster: "", position: "", size: 0,
    text: "", svg: "", svgBlocks: 0,
  }
  media.text = withoutComments(source)
  const match = MEDIA_RE.exec(outsideCode(media.text))
  takeAllFences(media)
  if (!match) return media
  media.file = match[2] || match[3] || ""
  media.video = isVideoFile(media.file)
  media.text = media.text.slice(0, match.index) + media.text.slice(match.index + match[0].length)
  media.svg = ""
  media.svgBlocks = 0
  takeAllFences(media)
  media.span = !media.video && /^# /m.test(outsideCode(media.text))
  const explicitOverlay = parseFlags(media, match[1].trim())
  if (media.position) {
    // A placed image leaves the slide's own layout and colors alone.
    media.span = false
    media.overlay = 0
    if (!media.size) media.size = 10
    return media
  }
  media.overlay = (!media.video || media.span) && media.text.trim() ? 0.25 : 0
  // The default darkening comes first; an explicit overlay= replaces it.
  if (explicitOverlay !== null) media.overlay = explicitOverlay
  return media
}

function takeAllFences(media) {
  let taken = takeFence(media.text, "svg")
  while (taken) {
    if (!media.svg) media.svg = taken.body
    media.svgBlocks++
    media.text = taken.text
    taken = takeFence(media.text, "svg")
  }
}

function parseFlags(media, flags) {
  DIRECTIVE_RE.lastIndex = 0
  const first = DIRECTIVE_RE.exec(flags)
  if (!first || first.index !== 0) return null
  if (!["fit", "span", "loop", "muted"].includes(first[1]) && first[2] === undefined) return null
  let explicitOverlay = "", fit = false, span = false
  DIRECTIVE_RE.lastIndex = 0
  let match
  while ((match = DIRECTIVE_RE.exec(flags))) {
    let value = match[2] === undefined ? "" : match[2]
    if (value.startsWith('"'))
      value = value.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, "\\")
    switch (match[1]) {
      case "span": media.span = true; span = true; break
      case "fit": media.span = false; fit = true; break
      case "loop": media.loop = value !== "false"; break
      case "muted": media.muted = value !== "false"; break
      case "autoplay": media.autoplay = value !== "false"; break
      case "overlay": explicitOverlay = value; break
      case "position": media.position = value in PLACEMENTS ? value : "center"; break
      case "size": {
        const number = /^(\d{1,3}(?:\.\d+)?)%$/.exec(value)
        media.size = number ? parseFloat(number[1]) : 0
        if (media.size < 1 || media.size > 100) media.size = 0
        if (!media.position) media.position = "center"
        break
      }
      case "background": media.background = value; break
      case "poster": media.poster = value; break
      // "alt" and anything unrecognized is ignored here; Hype check reports it.
    }
  }
  if (fit && span) media.span = false
  // A background choice implies fitting unless span was explicitly requested.
  if (!span && (media.background === "blur" || media.background === "auto"))
    media.span = false
  if (explicitOverlay === "") return null
  const opacity = parseFloat(explicitOverlay)
  return !Number.isNaN(opacity) && opacity >= 0 && opacity <= 1 ? opacity : null
}

// --- Deck rendering --------------------------------------------------------

// The Markdown travels with both exports. The folder export renders it into
// slides; the one-file export carries it beside slide pictures as the editable
// source. Its directives still steer navigation there, so read it once for both.
function readSourceDeck() {
  const sourceBlock = document.getElementById("source")
  if (!sourceBlock) return null
  const source = sourceBlock.textContent.replace(/<\\\//g, "</").trim() + "\n"
  const paletteTag = document.getElementById("palette")
  const palette = paletteTag ? JSON.parse(paletteTag.textContent) : {}
  return { source, palette, parsed: parseDeck(source) }
}

function renderMarkdownDeck(deck) {
  if (!deck) return
  // The one-file export embeds its Markdown the same way, beside slides that are
  // already pictures. There the block is the editable source for anyone who
  // receives the file, not a second deck to render on top of the first.
  if (stage.querySelector("section.slide")) return
  const palette = deck.palette
  const parsed = deck.parsed
  for (const key of ["background", "foreground", "accent", "green", "red", "yellow", "magenta", "cyan", "dark_foreground"]) {
    const value = scalar(parsed.header, "color_" + key)
    if (value && COLOR_RE.test(value)) palette[key] = value
  }
  const font = scalar(parsed.header, "font")
  if (font) palette.font = font
  const title = scalar(parsed.header, "title")
  if (title) document.title = title
  applyTheme(palette)
  const fits = []
  for (const slide of parsed.slides) {
    const section = document.createElement("section")
    section.className = "slide"
    section.setAttribute("aria-label", slideTitle(slide))
    stage.insertBefore(section, prev)
    renderSlide(section, slide, { ...palette }, fits)
  }
  // Fonts arrive after first paint; text fitted with a fallback may not fit.
  document.fonts.ready.then(() => fits.forEach(fit => fit()))
}

function applyTheme(palette) {
  const root = document.documentElement
  for (const key of ["background", "foreground", "accent", "green", "red", "yellow", "magenta", "cyan", "dark_foreground"])
    root.style.setProperty("--" + key.replace(/_/g, "-"), palette[key])
  root.style.setProperty("--font", `"${palette.font || "JetBrains Mono"}"`)
}

function slideTitle(text) {
  for (const raw of text.split("\n")) {
    const line = raw.trim()
    if (line.startsWith("#")) return line.slice(line.indexOf(" ") + 1).trim()
  }
  const first = text.split("\n")
    .map(l => l.trim())
    .find(l => l && !lineIsFence(l) && !l.startsWith("<!--"))
  return (first || "").replace(/^(>|[-*+]|\d+\.)\s+/, "")
}

function lineIsFence(line) {
  return FENCE.test(line)
}

function renderSlide(section, source, palette, fits) {
  const canvas = document.createElement("div")
  canvas.className = "canvas"
  section.appendChild(canvas)
  const bg = slideProperty(source, "background"), fgProperty = slideProperty(source, "foreground")
  if (COLOR_RE.test(bg)) palette.background = bg
  if (COLOR_RE.test(fgProperty)) palette.foreground = fgProperty
  canvas.style.background = palette.background
  const media = parseMedia(source)
  const text = media.text.trim()
  let area = TEXT_AREA
  const svgRect = media.svg && !media.file ? (text ? BELOW_AREA : FILL_AREA) : null
  if (svgRect) {
    drawSvg(canvas, svgRect, media.svg, palette)
    if (text) area = BAND_AREA
  }
  if (media.file && drawMedia(canvas, media, text, palette, COLOR_RE.test(fgProperty)))
    area = BAND_AREA
  const layout = text ? drawText(canvas, area, source, media, palette, fits) : null
  // Keep what a later canvas pass needs, after media adjusted the palette and
  // after the DOM fitted the type. paintSlideToCanvas never changes either.
  slideStates.set(section, { media, text, area, svgRect, layout, palette: { ...palette } })
}

function drawSvg(canvas, rect, svg, palette) {
  const box = document.createElement("div")
  box.className = "svg-box"
  placeAt(box, rect)
  // A drawing comes from whoever shared the deck, so it must not enter the page
  // as markup: scripts, event handlers, and <style> would then run with the
  // deck's privileges. Browsers draw an SVG image without scripts or access to
  // the page, which is also how the editor's own preview treats it.
  const drawing = document.createElement("img")
  drawing.alt = ""
  drawing.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(themedSvg(svg, palette))
  box.appendChild(drawing)
  canvas.appendChild(box)
}

function themedSvg(svg, palette) {
  let themed = svg.replace(/var\(\s*--([a-z_-]+)\s*\)/g, (whole, name) =>
    palette[name.replace(/-/g, "_")] || whole)
  const root = /<svg\b([^>]*)>/.exec(themed)
  if (root) {
    const attributes = root[1]
    let defaults = ""
    if (!/(^|\s)xmlns\s*=/.test(attributes)) defaults += ' xmlns="http://www.w3.org/2000/svg"'
    if (!/(^|\s)color\s*=/.test(attributes)) defaults += ` color="${palette.foreground}"`
    if (!/(^|\s)fill\s*=/.test(attributes)) defaults += ` fill="${palette.foreground}"`
    if (!/(^|\s)font-family\s*=/.test(attributes)) defaults += ` font-family="${palette.font || "JetBrains Mono"}"`
    themed = themed.slice(0, root.index + 4) + defaults + themed.slice(root.index + 4)
  }
  return themed
}

function drawMedia(canvas, media, text, palette, foregroundSet) {
  // Returns true when the slide's text moves to the band above the media.
  const placed = !!media.position
  const rect = mediaRect(media, text)
  const src = assetPath(media.file, media.video)
  const poster = media.poster ? assetPath(media.poster, false) : ""
  // A blurred copy of the picture fills the space around it.
  if (media.background === "blur" && !media.span && !placed) {
    const backdrop = document.createElement("div")
    backdrop.className = "backdrop"
    const blur = media.video && !poster ? document.createElement("video") : document.createElement("img")
    blur.src = poster || src
    if (blur.tagName === "VIDEO") { blur.preload = "metadata"; blur.muted = true }
    backdrop.appendChild(blur)
    canvas.appendChild(backdrop)
  }
  const element = media.video ? document.createElement("video") : document.createElement("img")
  element.className = "media" + (media.span ? " cover" : "")
  placeAt(element, rect)
  if (media.video) {
    element.src = src + "#t=0.1"
    element.preload = "metadata"
    element.playsInline = true
    if (poster) element.poster = poster
    if (media.loop) element.loop = true
    element.muted = media.muted
    if (!media.autoplay) element.controls = true
    element.dataset.autoplay = String(media.autoplay)
  } else {
    element.src = src
    element.alt = ""
    // Text over a picture reads over a softened copy, as in Hype.
    if (!media.span && !placed && text) element.classList.add("soft")
  }
  element.onerror = () => {
    element.remove()
    const notice = document.createElement("div")
    notice.className = "missing"
    notice.textContent = "Missing media\n" + media.file
    placeAt(notice, rect)
    canvas.appendChild(notice)
  }
  canvas.appendChild(element)
  // A chosen background color fills the slide, with ink that reads on it.
  if (!media.span && !placed && media.background !== "theme" && COLOR_RE.test(media.background)) {
    canvas.style.background = media.background
    if (!foregroundSet) {
      const ink = luminance(media.background) > 0.55 ? "#161616" : "#ffffff"
      palette.foreground = ink
      palette.accent = ink
    }
  }
  if (!placed && (!media.video || media.span)) {
    if (media.overlay > 0) {
      const overlay = document.createElement("div")
      overlay.className = "overlay"
      overlay.style.background = `rgba(0, 0, 0, ${media.overlay})`
      canvas.appendChild(overlay)
    }
    if (!foregroundSet && text) palette.foreground = "#ffffff"
  } else if (!placed && text) {
    return true // Text takes the band above a video.
  }
  // Hype fills the margins around a picture with its own edge color. Reading
  // pixels needs canvas access, which file:// pages are denied, so this only
  // takes effect where the browser allows it.
  if (!media.video && !media.span && !placed && media.background !== "theme" && !COLOR_RE.test(media.background))
    edgeColor(element).then(color => { if (color) canvas.style.background = color })
  return false
}

function luminance(hex) {
  const red = parseInt(hex.slice(1, 3), 16) / 255
  const green = parseInt(hex.slice(3, 5), 16) / 255
  const blue = parseInt(hex.slice(5, 7), 16) / 255
  return red * 0.2126 + green * 0.7152 + blue * 0.0722
}

async function edgeColor(image) {
  try {
    if (!image.complete)
      await new Promise((resolve, reject) => {
        image.addEventListener("load", resolve, { once: true })
        image.addEventListener("error", reject, { once: true })
      })
    if (!image.naturalWidth) return null
    const canvas = document.createElement("canvas")
    canvas.width = 64
    canvas.height = 36
    const context = canvas.getContext("2d", { willReadFrequently: true })
    context.drawImage(image, 0, 0, 64, 36)
    const pixels = context.getImageData(0, 0, 64, 36).data // Throws on file:// taint
    const votes = new Map()
    // Quantized edge votes, tolerating compression noise, as in Hype.
    for (let i = 0; i < 64; i++) {
      const x = i * (canvas.width - 1) / 63, y = i * (canvas.height - 1) / 63
      for (const [px, py] of [[x, 0], [x, canvas.height - 1], [0, y], [canvas.width - 1, y]]) {
        const offset = (Math.round(py) * canvas.width + Math.round(px)) * 4
        if (pixels[offset + 3] < 240) continue
        const key = ((pixels[offset] >> 4) << 8) | ((pixels[offset + 1] >> 4) << 4) | (pixels[offset + 2] >> 4)
        const entry = votes.get(key) || { count: 0, red: 0, green: 0, blue: 0 }
        entry.count++
        entry.red += pixels[offset]
        entry.green += pixels[offset + 1]
        entry.blue += pixels[offset + 2]
        votes.set(key, entry)
      }
    }
    let best = null
    for (const entry of votes.values())
      if (!best || entry.count > best.count) best = entry
    if (!best || best.count < 128) return null
    const toHex = value => Math.round(value / best.count).toString(16).padStart(2, "0")
    return `#${toHex(best.red)}${toHex(best.green)}${toHex(best.blue)}`
  } catch {
    return null
  }
}

// The picture's rectangle on the slide. drawMedia and the rasterizer share it,
// so a painted slide contains the same picture in the same place.
function mediaRect(media, text) {
  if (media.span) return { left: 0, top: 0, width: SLIDE_W, height: SLIDE_H }
  if (media.position) return placedRect(media)
  return !media.video || !text ? FILL_AREA : BELOW_AREA
}

function placedRect(media) {
  const width = SLIDE_W * media.size / 100, height = SLIDE_H * media.size / 100
  // The margin shrinks as the box grows, so large images stay on the slide.
  const margin = Math.min(60, (SLIDE_H - height) / 2)
  const within = { left: margin, top: margin, width: SLIDE_W - 2 * margin, height: SLIDE_H - 2 * margin }
  const [horizontal, vertical] = PLACEMENTS[media.position] || PLACEMENTS.center
  const left = horizontal === "left" ? within.left
    : horizontal === "right" ? within.left + within.width - width
    : within.left + (within.width - width) / 2
  const top = vertical === "top" ? within.top
    : vertical === "bottom" ? within.top + within.height - height
    : within.top + (within.height - height) / 2
  return { left, top, width, height }
}

function placeAt(element, rect) {
  element.style.left = rect.left + "px"
  element.style.top = rect.top + "px"
  element.style.width = rect.width + "px"
  element.style.height = rect.height + "px"
}

// --- Text ------------------------------------------------------------------

const TABLE_DELIMITER = /^ {0,3}(?=[^\n]*\|)\|? *:?-+:? *(?:\| *:?-+:? *)*\|? *$/

// A pipe alone does not make a table: the line below has to be a delimiter row.
function startsTable(lines, index) {
  return lines[index].includes("|") && index + 1 < lines.length && TABLE_DELIMITER.test(lines[index + 1])
}

function drawText(canvas, area, source, media, palette, fits) {
  const container = document.createElement("div")
  container.className = "text"
  placeAt(container, area)
  const content = document.createElement("div")
  content.className = "content"
  container.appendChild(content)
  const code = /^ {0,3}(`{3,}|~{3,})/m.test(media.text)
  const quote = media.text.trimStart().startsWith(">")
  const list = /^\s*([-*+] |\d+[.)] )/m.test(media.text)
  const lines = media.text.split("\n")
  const table = lines.some(line => line.includes("|") && TABLE_DELIMITER.test(line))
  const tableOnly = table && !lines.some(line => /\S/.test(line) && !line.includes("|"))
  let centered = tableOnly || !(code || quote || list || table)
  const alignment = slideProperty(source, "alignment")
  if (alignment === "left") centered = false
  if (alignment === "center") centered = true
  if (centered) container.classList.add("centered")
  const stack = media.text.includes("\n") && !media.text.trimStart().startsWith("#") && !quote && !list && !code
  let high = code ? 56 : quote ? 64 : list || table ? 72 : stack ? 128 : 76
  if (media.video && !media.span) high = 48
  const blocks = parseBlocks(media.text)
  content.style.color = palette.foreground
  renderBlocks(content, blocks)
  let ceiling = high
  const slide = canvas.closest(".slide")
  const fit = () => {
    // Slides render while display: none, and a hidden element measures zero, which
    // would let every size fit. Lay the slide out invisibly for the search, so its
    // text is measured at its true size, then restore it.
    if (slide) slide.classList.add("measuring")
    let low = 8
    const fitsAt = size => {
      content.style.fontSize = size + "px"
      return content.offsetHeight <= area.height && content.scrollWidth <= area.width + 1
    }
    if (fitsAt(ceiling)) low = ceiling
    else {
      for (let i = 0; i < 9; i++) {
        const size = (low + ceiling) / 2
        if (fitsAt(size)) low = size
        else ceiling = size
      }
    }
    content.style.fontSize = low + "px"
    if (slide) slide.classList.remove("measuring")
  }
  // Attach before fitting: a detached element measures zero, which would let
  // every size fit and leave the text at its ceiling.
  canvas.appendChild(container)
  fit()
  fits.push(fit)
  // The fitted size is now in content's inline style; the rasterizer reads it
  // from there rather than fitting again.
  return { blocks, centered }
}

// Ordinary line breaks stay visible on the slide, as Hype shows them. Markdown's
// hard-break markers (a trailing backslash or two trailing spaces) are not text, and
// every other newline becomes a <br>: HTML alone would fold it into a space.
function appendLines(parent, lines) {
  // One pass over the whole paragraph, so inline markup can span its lines.
  inline(parent, paragraphText(lines))
}

function paragraphText(lines) {
  return lines.map(line => line.replace(/ +$/, "").replace(/\\$/, "")).join("\n")
}

// Markdown becomes a neutral block list—styled runs with no DOM in them—so the
// page can draw the same text twice: as HTML for fitting and presentation, and
// onto a canvas for the folder export's rasterizer.
function parseBlocks(text) {
  const lines = text.split("\n")
  const blocks = []
  let i = 0
  while (i < lines.length) {
    const line = lines[i]
    if (!line.trim()) { i++; continue }
    const fence = FENCE.exec(line)
    if (fence) {
      let j = i + 1
      for (; j < lines.length; j++) {
        const close = FENCE.exec(lines[j])
        if (close && close[1][0] === fence[1][0] && close[1].length >= fence[1].length && !close[2].trim()) break
      }
      const body = (j >= lines.length ? lines.slice(i + 1) : lines.slice(i + 1, j)).join("\n")
      const language = fence[2].trim().split(/\s+/)[0] || ""
      if (language.toLowerCase() !== "svg")
        blocks.push({ kind: "code", language, text: body })
      i = j >= lines.length ? lines.length : j + 1
      continue
    }
    const heading = /^ {0,3}(#{1,6}) +(.*)$/.exec(line)
    if (heading) {
      blocks.push({ kind: "heading", level: heading[1].length, runs: parseInline(heading[2]) })
      i++
      continue
    }
    if (line.trimStart().startsWith(">")) {
      const paragraphs = []
      while (i < lines.length && lines[i].trimStart().startsWith(">")) {
        const body = lines[i].trimStart().replace(/^>\s?/, "")
        paragraphs.push({ attribution: body.startsWith("—"), runs: parseInline(body) })
        i++
      }
      blocks.push({ kind: "quote", paragraphs })
      continue
    }
    const item = /^ {0,3}([-*+]|\d+[.)]) +(.*)$/.exec(line)
    if (item) {
      // The marker decides the list kind, never the text after it.
      const ordered = /\d/.test(item[1])
      const items = []
      while (i < lines.length) {
        const entry = /^ {0,3}([-*+]|\d+[.)]) +(.*)$/.exec(lines[i])
        if (!entry) break
        items.push(parseInline(entry[2]))
        i++
      }
      blocks.push({ kind: "list", ordered, items })
      continue
    }
    if (startsTable(lines, i)) {
      blocks.push(parseTable(lines, i))
      while (i < lines.length && lines[i].includes("|") && lines[i].trim()) i++
      continue
    }
    const body = []
    while (i < lines.length && lines[i].trim() && !lineIsFence(lines[i]) &&
           !/^ {0,3}#{1,6} /.test(lines[i]) && !lines[i].trimStart().startsWith(">") &&
           !/^ {0,3}([-*+]|\d+[.)]) /.test(lines[i]) && !startsTable(lines, i))
      body.push(lines[i++])
    blocks.push({ kind: "paragraph", runs: parseInline(paragraphText(body)) })
  }
  return blocks
}

// The DOM backend. It keeps the exact structure and classes the stylesheet
// was written for; tests and the fit depend on both.
function renderBlocks(content, blocks) {
  for (const block of blocks) {
    switch (block.kind) {
      case "code": {
        const pre = document.createElement("pre")
        const codeElement = document.createElement("code")
        codeElement.innerHTML = highlight(block.text, block.language)
        pre.appendChild(codeElement)
        content.appendChild(pre)
        break
      }
      case "heading": {
        const element = document.createElement("h" + block.level)
        appendRuns(element, block.runs)
        content.appendChild(element)
        break
      }
      case "quote": {
        const quoteElement = document.createElement("blockquote")
        for (const paragraph of block.paragraphs) {
          const element = document.createElement("p")
          if (paragraph.attribution) element.className = "attribution"
          appendRuns(element, paragraph.runs)
          quoteElement.appendChild(element)
        }
        content.appendChild(quoteElement)
        break
      }
      case "list": {
        const listElement = document.createElement(block.ordered ? "ol" : "ul")
        for (const runs of block.items) {
          const li = document.createElement("li")
          appendRuns(li, runs)
          listElement.appendChild(li)
        }
        content.appendChild(listElement)
        break
      }
      case "table":
        content.appendChild(renderTable(block))
        break
      case "paragraph": {
        const paragraph = document.createElement("p")
        appendRuns(paragraph, block.runs)
        content.appendChild(paragraph)
        break
      }
    }
  }
}

function buildBlocks(content, text) {
  renderBlocks(content, parseBlocks(text))
}

function parseTable(lines, start) {
  const cells = line => {
    let text = line.trim()
    if (text.startsWith("|")) text = text.slice(1)
    if (text.endsWith("|")) text = text.slice(0, -1)
    return text.split(/(?<!\\)\|/)
  }
  const alignments = cells(lines[start + 1]).map(cell => {
    const marker = cell.trim()
    return marker.startsWith(":") && marker.endsWith(":") ? "center" : marker.endsWith(":") ? "right" : "left"
  })
  const head = cells(lines[start]).map(cell => parseInline(padCell(cell)))
  const rows = []
  for (let i = start + 2; i < lines.length && lines[i].includes("|") && lines[i].trim(); i++)
    rows.push(cells(lines[i]).map(cell => parseInline(padCell(cell))))
  return { kind: "table", alignments, head, rows }
}

function renderTable(block) {
  const table = document.createElement("table")
  const head = document.createElement("thead")
  const headRow = document.createElement("tr")
  block.head.forEach((runs, column) => {
    const th = document.createElement("th")
    th.style.textAlign = block.alignments[column] || "left"
    appendRuns(th, runs)
    headRow.appendChild(th)
  })
  head.appendChild(headRow)
  table.appendChild(head)
  const body = document.createElement("tbody")
  for (const row of block.rows) {
    const tr = document.createElement("tr")
    row.forEach((runs, column) => {
      const td = document.createElement("td")
      td.style.textAlign = block.alignments[column] || "left"
      appendRuns(td, runs)
      tr.appendChild(td)
    })
    body.appendChild(tr)
  }
  table.appendChild(body)
  return table
}

// Markdown trims table cells; keep the separation that reads as padding.
function padCell(cell) {
  const leading = Math.max(0, cell.length - cell.trimStart().length - 1)
  const trailing = Math.max(0, cell.length - cell.trimEnd().length - 1)
  return " ".repeat(leading) + cell.trim() + " ".repeat(trailing)
}

const INLINE_SOURCE =
  "(`+)([^`]|`(?!`))*?\\1|\\*\\*(.+?)\\*\\*|__(.+?)__|\\*(.+?)\\*|_([^_]+)_|\\[([^\\]]*)\\]\\(([^)\\s]+)\\)"

function parseInline(text) {
  const pattern = new RegExp(INLINE_SOURCE, "g")
  const nodes = []
  // Text between constructs carries the paragraph's newlines: on the slide they are
  // breaks, which HTML would otherwise fold into spaces.
  const literal = value => value.split("\n").forEach((part, index) => {
    if (index) nodes.push({ kind: "br" })
    if (part) nodes.push({ kind: "text", text: part })
  })
  let last = 0
  let match
  while ((match = pattern.exec(text))) {
    if (match.index > last) literal(text.slice(last, match.index))
    const [, ticks, , bold, boldUnderscore, italic, underline, linkText, linkTarget] = match
    if (ticks) {
      nodes.push({ kind: "code", text: match[0].slice(ticks.length, -ticks.length) })
    } else if (bold !== undefined || boldUnderscore !== undefined) {
      nodes.push({ kind: "strong", children: parseInline(bold ?? boldUnderscore) })
    } else if (italic !== undefined) {
      nodes.push({ kind: "em", children: parseInline(italic) })
    } else if (underline !== undefined) {
      nodes.push({ kind: "u", children: parseInline(underline) })
    } else if (linkText !== undefined) {
      const href = /^(https?:|mailto:|#|\.{0,2}\/|[a-z0-9._-]+\.md$)/i.test(linkTarget)
        ? linkTarget : ""
      nodes.push({ kind: "link", href, text: linkText })
    }
    last = match.index + match[0].length
  }
  if (last < text.length) literal(text.slice(last))
  return nodes
}

function appendRuns(parent, nodes) {
  for (const node of nodes) {
    switch (node.kind) {
      case "text":
        parent.appendChild(document.createTextNode(node.text))
        break
      case "br":
        parent.appendChild(document.createElement("br"))
        break
      case "code": {
        const element = document.createElement("code")
        element.textContent = node.text
        parent.appendChild(element)
        break
      }
      case "strong":
      case "em":
      case "u": {
        const element = document.createElement(node.kind)
        appendRuns(element, node.children)
        parent.appendChild(element)
        break
      }
      case "link": {
        const element = document.createElement("a")
        if (node.href) element.href = node.href
        element.textContent = node.text
        parent.appendChild(element)
        break
      }
    }
  }
}

function inline(parent, text) {
  appendRuns(parent, parseInline(text))
}

// --- Code highlighting -----------------------------------------------------
// A compact tokenizer in the editor's colors: keywords magenta, strings green,
// numbers red, comments dim, calls accent, capitalized names yellow.

const KEYWORDS = {
  common: "as break case catch class const continue default defer delete do elif else enum except finally for func function if import in is lambda let new not or pass raise return struct super switch this throw try type typeof var while with yield",
  javascript: "await async export extends from implements instanceof interface of static package get set",
  python: "and assert async await del global nonlocal None True False self",
  ruby: "begin end elsif ensure module nil redo rescue retry true false unless until when",
  rust: "crate dyn impl loop match mod mut pub ref trait unsafe use where async await move",
  go: "chan fallthrough go goto interface map package range select nil",
  shell: "then fi done esac function local select until time coproc",
  sql: "asc by create desc drop from group having index insert into join left null on order primary select set table top union update values view where",
}

// Tokens carry their color class to both backends: the DOM wraps them in spans,
// the canvas picks a fill color per class.
function highlightRuns(code, language) {
  language = (language || "").toLowerCase().split(" ")[0]
  const hash = ["sh", "bash", "shell", "zsh", "python", "py", "ruby", "rb", "yaml", "yml", "toml", "ini", "conf", "r", "perl", "makefile", "dockerfile"].includes(language)
  const dash = ["sql", "lua", "haskell", "hs", "elm", "ada"].includes(language)
  const words = new Set(KEYWORDS.common.split(" ").concat(KEYWORDS.javascript.split(" ")))
  for (const [languages, list] of [[["python", "py"], "python"], [["ruby", "rb"], "ruby"],
                                   [["rust", "rs"], "rust"], [["go", "golang"], "go"],
                                   [["sh", "bash", "shell", "zsh"], "shell"], [["sql"], "sql"]])
    if (languages.includes(language)) KEYWORDS[list].split(" ").forEach(word => words.add(word))
  const patterns = []
  if (hash) patterns.push([/#[^\n]*/y, "c"])
  if (dash) patterns.push([/--[^\n]*/y, "c"])
  if (!dash) patterns.push([/\/\/[^\n]*/y, "c"])
  patterns.push([/\/\*[\s\S]*?\*\//y, "c"])
  patterns.push([/"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`/y, "s"])
  patterns.push([/\b\d[\w.]*\b/y, "n"])
  patterns.push([/[A-Za-z_$][\w$]*/y, null])
  const runs = []
  const push = (text, cls) => { if (text) runs.push(cls ? { text, cls } : { text }) }
  let plain = ""
  const flush = () => { if (plain) { push(plain); plain = "" } }
  let i = 0
  while (i < code.length) {
    let matched = false
    for (const [pattern, cls] of patterns) {
      pattern.lastIndex = i
      const match = pattern.exec(code)
      if (!match || match.index !== i) continue
      matched = true
      flush()
      if (!cls) {
        const word = match[0]
        if (words.has(word)) push(word, "k")
        else if (/^[A-Z]/.test(word)) push(word, "t")
        else if (/^\s*\(/.test(code.slice(i + word.length))) push(word, "f")
        else push(word)
      } else push(match[0], cls)
      i += match[0].length
      break
    }
    if (!matched) plain += code[i++]
  }
  flush()
  return runs
}

function highlight(code, language) {
  return highlightRuns(code, language).map(run =>
    run.cls ? `<span class="${run.cls}">${escapeHtml(run.text)}</span>` : escapeHtml(run.text)
  ).join("")
}

function escapeHtml(text) {
  return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

// --- Canvas rasterizer -----------------------------------------------------
//
// The folder export can also draw each slide as a 1920 × 1080 bitmap. The DOM
// above has already laid out and fitted the text, so this pass never changes a
// font size: it reads the fitted metrics and draws them with fillText and
// measureText. Media uses the same rectangles drawMedia placed in the DOM.

// The stylesheet's defaults, for palettes that omit a token color.
const DEFAULT_COLORS = {
  background: "#1a1b26", foreground: "#c0caf5", accent: "#7aa2f7", green: "#9ece6a",
  red: "#f7768e", yellow: "#e0af68", magenta: "#bb9af7", cyan: "#7dcfff",
  dark_foreground: "#787c99",
}

// Paints into `output` when one is supplied, so a cached texture can be
// repainted in place once a picture or font arrives without changing identity.
function paintSlideToCanvas(section, output = document.createElement("canvas")) {
  output.width = SLIDE_W
  output.height = SLIDE_H
  const context = output.getContext("2d")
  const state = slideStates.get(section)
  const box = section.querySelector(".canvas") || section
  const computed = getComputedStyle(box).backgroundColor
  context.fillStyle = computed && computed !== "rgba(0, 0, 0, 0)" && computed !== "transparent"
    ? computed : colorOf(state, "background")
  context.fillRect(0, 0, SLIDE_W, SLIDE_H)
  // A baked deck's section is one picture already; draw it as one.
  if (!state) {
    const image = section.querySelector("img")
    if (image) paintImage(context, image, { left: 0, top: 0, width: SLIDE_W, height: SLIDE_H }, "cover")
    return output
  }
  const { media, text, layout, svgRect } = state
  const placed = !!media.position
  // drawMedia's order: blurred backdrop, picture, darkening, text last.
  if (media.background === "blur" && !media.span && !placed) {
    const backdrop = section.querySelector(".backdrop img, .backdrop video")
    if (backdrop) {
      const width = SLIDE_W * 1.1, height = SLIDE_H * 1.1
      paintImage(context, backdrop,
                 { left: (SLIDE_W - width) / 2, top: (SLIDE_H - height) / 2, width, height },
                 "cover", 48)
    }
  }
  if (media.file) {
    const element = section.querySelector(".media")
    const rect = mediaRect(media, text)
    if (element)
      paintImage(context, element, rect, media.span ? "cover" : "contain",
                 element.classList.contains("soft") ? 2 : 0)
    else
      paintMissing(context, rect, media.file, state)
  }
  if (!placed && media.overlay > 0) {
    context.fillStyle = `rgba(0, 0, 0, ${media.overlay})`
    context.fillRect(0, 0, SLIDE_W, SLIDE_H)
  }
  if (svgRect) {
    const drawing = section.querySelector(".svg-box img")
    if (drawing) paintImage(context, drawing, svgRect, "contain")
  }
  if (text && layout) paintText(context, section, layout, state)
  return output
}

function colorOf(state, key) {
  return (state && state.palette[key]) || DEFAULT_COLORS[key] || "#000"
}

function paintImage(context, element, rect, fit, blur = 0) {
  const video = element.tagName === "VIDEO"
  if (video ? element.readyState < 2 : !element.complete) return false
  const width = video ? element.videoWidth : element.naturalWidth
  const height = video ? element.videoHeight : element.naturalHeight
  if (!width || !height) return false
  const scale = fit === "cover"
    ? Math.max(rect.width / width, rect.height / height)
    : Math.min(rect.width / width, rect.height / height)
  const drawnWidth = width * scale, drawnHeight = height * scale
  context.save()
  if (blur > 0 && "filter" in context) context.filter = `blur(${blur}px)`
  context.drawImage(element, rect.left + (rect.width - drawnWidth) / 2,
                    rect.top + (rect.height - drawnHeight) / 2, drawnWidth, drawnHeight)
  context.restore()
  return true
}

function paintMissing(context, rect, file, state) {
  context.save()
  context.fillStyle = colorOf(state, "accent")
  context.font = "32px system-ui, sans-serif"
  context.textAlign = "center"
  context.textBaseline = "middle"
  const lines = ["Missing media", file]
  lines.forEach((line, index) =>
    context.fillText(line, rect.left + rect.width / 2,
                    rect.top + rect.height / 2 + (index - (lines.length - 1) / 2) * 40))
  context.restore()
}

function paintText(context, section, layout, state) {
  const textElement = section.querySelector(".text")
  const content = textElement && textElement.querySelector(".content")
  if (!textElement || !content) return
  const elements = [...content.children]
  // A hidden slide has no layout boxes, so lay it out invisibly for this read,
  // as the fit does. Nothing is painted while measuring, and the class is gone
  // before paintText returns.
  const hidden = getComputedStyle(section).display === "none"
  if (hidden) section.classList.add("measuring")
  try {
    const palette = state.palette
    const accent = colorOf(state, "accent")
    const originX = textElement.offsetLeft, originY = textElement.offsetTop
    // The fitted size lives on .content; every child inherits it, so the DOM
    // stays the authority and this pass only scales from it (headings, quotes).
    const fitted = parseFloat(getComputedStyle(content).fontSize) || 76
    const codeElement = content.querySelector("code")
    const codeFamily = codeElement
      ? getComputedStyle(codeElement).fontFamily : getComputedStyle(content).fontFamily
    const tokenColors = {
      k: colorOf(state, "magenta"), s: colorOf(state, "green"), n: colorOf(state, "red"),
      c: colorOf(state, "dark_foreground"), f: colorOf(state, "accent"),
      t: colorOf(state, "yellow"), p: colorOf(state, "cyan"),
    }
    layout.blocks.forEach((block, index) => {
      const element = elements[index]
      if (!element) return
      switch (block.kind) {
        case "code":
          paintCode(context, element, block, originX, originY, fitted, codeFamily, tokenColors)
          break
        case "table":
          paintTable(context, element, block, originX, originY, fitted, accent, codeFamily)
          break
        case "list":
          paintList(context, element, block, originX, originY, fitted, accent, codeFamily)
          break
        case "quote":
          paintQuote(context, element, block, originX, originY, fitted, accent, codeFamily)
          break
        default:
          paintLines(context, element, block.runs, originX, originY, fitted, accent,
                     codeFamily, block.kind === "heading")
      }
    })
    // Keep the compiler honest: the fitted size is read above even when no
    // block needs it, because that is the metric this pass is required to use.
    void fitted
    void palette
  } finally {
    if (hidden) section.classList.remove("measuring")
  }
}

function canvasMetrics(element, fitted) {
  const style = getComputedStyle(element)
  const size = parseFloat(style.fontSize) || fitted
  const lineHeight = parseFloat(style.lineHeight)
  return {
    size,
    lineHeight: Number.isFinite(lineHeight) && lineHeight > 0 ? lineHeight : size * 1.15,
    family: style.fontFamily || "monospace",
    weight: style.fontWeight || "400",
    italic: style.fontStyle === "italic",
    align: style.textAlign === "center" ? "center"
      : style.textAlign === "right" || style.textAlign === "end" ? "right" : "left",
    color: style.color || "#fff",
  }
}

function canvasTokens(nodes, style, tokens) {
  for (const node of nodes) {
    switch (node.kind) {
      case "text":
        splitTokens(node.text, style, tokens)
        break
      case "br":
        tokens.push({ br: true })
        break
      case "code":
        splitTokens(node.text, { ...style, code: true }, tokens)
        break
      case "strong":
        canvasTokens(node.children, { ...style, bold: true }, tokens)
        break
      case "em":
        canvasTokens(node.children, { ...style, italic: true }, tokens)
        break
      case "u":
        canvasTokens(node.children, { ...style, underline: true }, tokens)
        break
      case "link":
        splitTokens(node.text, { ...style, link: true }, tokens)
        break
    }
  }
}

function splitTokens(text, style, tokens) {
  for (const part of text.split(/(\s+)/)) {
    if (!part) continue
    if (/^\s+$/.test(part)) tokens.push({ text: " ", space: true, style })
    else tokens.push({ text: part, style })
  }
}

function setRunFont(context, metrics, style, codeFamily) {
  const weight = style.bold ? "700" : metrics.weight
  const italic = style.italic || metrics.italic ? "italic " : ""
  const family = style.code ? codeFamily : metrics.family
  context.font = `${italic}${weight} ${metrics.size}px ${family}`
}

function measureToken(context, metrics, token, codeFamily) {
  setRunFont(context, metrics, token.style, codeFamily)
  return context.measureText(token.text).width
}

function wrapTokens(context, tokens, metrics, maxWidth, codeFamily) {
  const lines = []
  let line = [], width = 0
  const finish = () => {
    // Trailing spaces neither occupy the line nor shift centered text.
    while (line.length && line[line.length - 1].space) line.pop()
    lines.push(line)
    line = []
    width = 0
  }
  for (const token of tokens) {
    if (token.br) { finish(); continue }
    const measured = { ...token, width: measureToken(context, metrics, token, codeFamily) }
    if (token.space) {
      if (line.length) { line.push(measured); width += measured.width }
      continue
    }
    if (line.length && width + measured.width > maxWidth) finish()
    line.push(measured)
    width += measured.width
  }
  finish()
  return lines
}

function canvasColor(style, metrics, accent) {
  if (style.color) return style.color
  if (style.link) return accent
  if (style.bold && !metrics.heading) return accent
  return metrics.color
}

function paintRunLines(context, metrics, lines, x, top, width, colorFor, codeFamily) {
  let y = top
  for (const line of lines) {
    const lineWidth = line.reduce((sum, token) => sum + token.width, 0)
    let cursor = metrics.align === "center" ? x + (width - lineWidth) / 2
      : metrics.align === "right" ? x + width - lineWidth : x
    for (const token of line) {
      if (token.space) { cursor += token.width; continue }
      setRunFont(context, metrics, token.style, codeFamily)
      context.fillStyle = colorFor(token.style)
      context.textAlign = "left"
      context.textBaseline = "middle"
      context.fillText(token.text, cursor, y + metrics.lineHeight / 2)
      if (token.style.underline)
        context.fillRect(cursor, y + metrics.lineHeight / 2 + metrics.size * 0.12,
                         token.width, Math.max(1, metrics.size * 0.06))
      cursor += token.width
    }
    y += metrics.lineHeight
  }
}

function paintLines(context, element, runs, originX, originY, fitted, accent, codeFamily, heading) {
  const metrics = canvasMetrics(element, fitted)
  metrics.heading = !!heading
  const style = getComputedStyle(element)
  const paddingLeft = parseFloat(style.paddingLeft) || 0
  const paddingRight = parseFloat(style.paddingRight) || 0
  const width = Math.max(0, element.clientWidth - paddingLeft - paddingRight)
  const tokens = []
  canvasTokens(runs, {}, tokens)
  const lines = wrapTokens(context, tokens, metrics, width, codeFamily)
  paintRunLines(context, metrics, lines, originX + element.offsetLeft + paddingLeft,
                originY + element.offsetTop + (parseFloat(style.paddingTop) || 0), width,
                token => canvasColor(token, metrics, accent), codeFamily)
}

function paintQuote(context, element, block, originX, originY, fitted, accent, codeFamily) {
  const paragraphs = [...element.children]
  block.paragraphs.forEach((paragraph, index) => {
    const p = paragraphs[index]
    if (p) paintLines(context, p, paragraph.runs, originX, originY, fitted, accent,
                     codeFamily, false)
  })
}

function paintList(context, element, block, originX, originY, fitted, accent, codeFamily) {
  const items = [...element.children]
  block.items.forEach((runs, index) => {
    const li = items[index]
    if (!li) return
    const metrics = canvasMetrics(li, fitted)
    const style = getComputedStyle(li)
    const paddingLeft = parseFloat(style.paddingLeft) || 0
    const width = Math.max(0, li.clientWidth - paddingLeft - (parseFloat(style.paddingRight) || 0))
    const tokens = []
    canvasTokens(runs, {}, tokens)
    const lines = wrapTokens(context, tokens, metrics, width, codeFamily)
    paintRunLines(context, metrics, lines, originX + li.offsetLeft + paddingLeft,
                  originY + li.offsetTop, width,
                  token => canvasColor(token, metrics, accent), codeFamily)
    // Markers sit outside the content box, as the stylesheet's list padding does.
    const marker = block.ordered ? `${index + 1}.` : "•"
    setRunFont(context, metrics, {}, codeFamily)
    context.fillStyle = metrics.color
    context.textAlign = "right"
    context.textBaseline = "middle"
    context.fillText(marker, originX + li.offsetLeft - metrics.size * 0.35,
                     originY + li.offsetTop + metrics.lineHeight / 2)
  })
}

function paintTable(context, element, block, originX, originY, fitted, accent, codeFamily) {
  const rowElements = [...element.querySelectorAll("tr")]
  const rows = [block.head, ...block.rows]
  rows.forEach((cells, rowIndex) => {
    const row = rowElements[rowIndex]
    if (!row) return
    ;[...row.children].forEach((cell, column) => {
      const runs = cells[column]
      if (!runs) return
      const metrics = canvasMetrics(cell, fitted)
      const style = getComputedStyle(cell)
      const paddingLeft = parseFloat(style.paddingLeft) || 0
      const paddingRight = parseFloat(style.paddingRight) || 0
      const paddingTop = parseFloat(style.paddingTop) || 0
      const width = Math.max(0, cell.clientWidth - paddingLeft - paddingRight)
      const tokens = []
      canvasTokens(runs, {}, tokens)
      const lines = wrapTokens(context, tokens, metrics, width, codeFamily)
      paintRunLines(context, metrics, lines, originX + cell.offsetLeft + paddingLeft,
                    originY + cell.offsetTop + paddingTop, width,
                    token => canvasColor(token, metrics, accent), codeFamily)
    })
    const style = getComputedStyle(row)
    const border = parseFloat(style.borderBottomWidth) || 0
    if (border > 0 && style.borderBottomStyle !== "none") {
      context.fillStyle = style.borderBottomColor
      context.fillRect(originX + row.offsetLeft,
                       originY + row.offsetTop + row.offsetHeight - border / 2,
                       row.offsetWidth, border)
    }
  })
}

function paintCode(context, element, block, originX, originY, fitted, codeFamily, tokenColors) {
  const style = getComputedStyle(element)
  const metrics = {
    size: parseFloat(style.fontSize) || fitted,
    lineHeight: parseFloat(style.lineHeight) || (parseFloat(style.fontSize) || fitted) * 1.2,
    family: style.fontFamily || codeFamily,
    weight: style.fontWeight || "400",
    italic: style.fontStyle === "italic",
    color: style.color || "#fff",
  }
  context.textAlign = "left"
  context.textBaseline = "middle"
  // Highlighting is a token stream; a newline splits it into drawable lines.
  const lines = [[]]
  for (const run of highlightRuns(block.text, block.language)) {
    run.text.split("\n").forEach((part, index) => {
      if (index) lines.push([])
      if (part) lines[lines.length - 1].push({ text: part, cls: run.cls })
    })
  }
  const x = originX + element.offsetLeft, top = originY + element.offsetTop
  lines.forEach((line, index) => {
    let cursor = x
    for (const part of line) {
      context.font = `${metrics.italic ? "italic " : ""}${metrics.weight} ${metrics.size}px ${metrics.family}`
      context.fillStyle = part.cls ? tokenColors[part.cls] : metrics.color
      context.fillText(part.text, cursor, top + index * metrics.lineHeight + metrics.lineHeight / 2)
      cursor += context.measureText(part.text).width
    }
  })
}

// --- Wiring ----------------------------------------------------------------

const sourceDeck = readSourceDeck()
renderMarkdownDeck(sourceDeck)

const slides = [...stage.querySelectorAll(".slide")]

// --- Slide textures --------------------------------------------------------
//
// The rasterizer's output, kept per slide for this load. A folder export's
// slides do not change while the page is open—editing the Markdown means
// reloading it—so one canvas per index is enough. A picture or font that
// arrives after the first paint repaints the same canvas in place, which keeps
// the identity a caller already holds and lets a transition upload it late.

const slideTextures = new Map()

function slideTexture(index) {
  if (!Number.isInteger(index) || index < 0 || index >= slides.length)
    return null
  const cached = slideTextures.get(index)
  if (cached)
    return cached
  const section = slides[index]
  const texture = paintSlideToCanvas(section)
  slideTextures.set(index, texture)
  // A texture painted before its pictures decoded is repainted, not replaced.
  const repaint = () => paintSlideToCanvas(section, texture)
  for (const element of section.querySelectorAll("img, video")) {
    if (element.tagName === "VIDEO") {
      if (element.readyState < 2)
        element.addEventListener("loadeddata", repaint, { once: true })
    } else if (!element.complete) {
      element.addEventListener("load", repaint, { once: true })
    }
  }
  return texture
}

// Text fitted with a fallback face is refitted once the real font loads, so a
// texture painted before that would carry the old metrics.
document.fonts.ready.then(() => {
  for (const [index, texture] of slideTextures)
    paintSlideToCanvas(slides[index], texture)
})

function scaleCanvases() {
  const scale = stage.clientWidth / SLIDE_W
  for (const canvas of stage.querySelectorAll(".canvas"))
    canvas.style.transform = `scale(${scale})`
}
scaleCanvases()
window.addEventListener("resize", scaleCanvases)

let current = -1

function fromHash() {
  const n = parseInt(location.hash.slice(1), 10)
  return Number.isInteger(n) ? n - 1 : 0
}

// --- Transitions -----------------------------------------------------------
//
// A transition draws the outgoing slide into one texture and the incoming slide
// into another, then mixes them with the shader named by the directives. The
// slides themselves swap instantly underneath; the overlay is the only thing
// animating. Any failure (no WebGL, an image that will not decode, an unknown
// shader) simply skips the animation and leaves the new slide in place.

const TRANSITION_VERTEX = `attribute vec2 position;
varying vec2 vUv;
void main() {
  vUv = position * 0.5 + 0.5;
  gl_Position = vec4(position, 0.0, 1.0);
}`

const TRANSITION_FRAGMENT_HEADER = `#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D fromTex;
uniform sampler2D toTex;
uniform float progress;
varying vec2 vUv;
`

const TRANSITION_FRAGMENT_FOOTER = `
void main() {
  gl_FragColor = transition(vUv);
}
`

// The shader source registry rides with the page, compiled by transitions.cpp.
function transitionShaders() {
  const block = document.getElementById("transitions")
  if (!block) return {}
  try {
    return JSON.parse(block.textContent)
  } catch {
    return {}
  }
}

// The names the editor knows. A deck may be opened without a shader block (a
// page with no transitions at all), but its header still resolves to a name;
// whether that name has a body to compile is the engine's problem, not the
// resolver's. Keep this list beside transitions.cpp's transitionNames().
const TRANSITION_NAMES = ["none", "crossfade", "dissolve", "liquid", "glitch"]

// A slide's directive wins over the deck's header, and "none" wins over both.
// An unknown name is ignored, as in the editor: the slide falls back to the
// deck's choice, and the deck to no animation.
function resolveTransition(index) {
  if (!sourceDeck) return { name: "none", duration: 0.6 }
  const known = name => TRANSITION_NAMES.includes(name)
  const slide = sourceDeck.parsed.slides[index] || ""
  const override = slideProperty(slide, "transition")
  const fallback = scalar(sourceDeck.parsed.header, "transition")
  const name = known(override) ? override : known(fallback) ? fallback : "none"
  const raw = scalar(sourceDeck.parsed.header, "transition-duration")
  const seconds = raw === null || raw === "" ? NaN : Number(raw)
  const duration = Number.isFinite(seconds) ? Math.min(4, Math.max(0.1, seconds)) : 0.6
  return { name, duration }
}

const transitions = (() => {
  let canvas = null, gl = null, quad = null, fromTex = null, toTex = null
  const programs = new Map()
  let frame = 0
  let generation = 0

  function setUp() {
    if (canvas) return gl !== null
    canvas = document.createElement("canvas")
    canvas.className = "transition"
    canvas.width = SLIDE_W
    canvas.height = SLIDE_H
    canvas.style.display = "none"
    gl = canvas.getContext("webgl", { alpha: false, antialias: false })
    if (!gl) return false
    // Source pictures have their origin at the top left; this flips them into
    // the texture coordinates the shaders use.
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true)
    gl.disable(gl.DEPTH_TEST)
    quad = gl.createBuffer()
    gl.bindBuffer(gl.ARRAY_BUFFER, quad)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]),
                  gl.STATIC_DRAW)
    fromTex = texture()
    toTex = texture()
    stage.insertBefore(canvas, prev)
    return true
  }

  function texture() {
    const id = gl.createTexture()
    gl.bindTexture(gl.TEXTURE_2D, id)
    // 1080p is not a power of two: clamp and filter linearly, with no mipmaps.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, SLIDE_W, SLIDE_H, 0,
                  gl.RGBA, gl.UNSIGNED_BYTE, null)
    return id
  }

  function compile(name) {
    if (programs.has(name)) return programs.get(name)
    const body = transitionShaders()[name]
    const program = body ? link(TRANSITION_VERTEX, TRANSITION_FRAGMENT_HEADER + body +
                                TRANSITION_FRAGMENT_FOOTER) : null
    programs.set(name, program)
    return program
  }

  function shader(type, source) {
    const id = gl.createShader(type)
    gl.shaderSource(id, source)
    gl.compileShader(id)
    if (gl.getShaderParameter(id, gl.COMPILE_STATUS)) return id
    gl.deleteShader(id)
    return null
  }

  function link(vertexSource, fragmentSource) {
    const vertex = shader(gl.VERTEX_SHADER, vertexSource)
    const fragment = shader(gl.FRAGMENT_SHADER, fragmentSource)
    if (!vertex || !fragment) return null
    const program = gl.createProgram()
    gl.attachShader(program, vertex)
    gl.attachShader(program, fragment)
    gl.linkProgram(program)
    gl.deleteShader(vertex)
    gl.deleteShader(fragment)
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      gl.deleteProgram(program)
      return null
    }
    program.position = gl.getAttribLocation(program, "position")
    program.from = gl.getUniformLocation(program, "fromTex")
    program.to = gl.getUniformLocation(program, "toTex")
    program.progress = gl.getUniformLocation(program, "progress")
    return program
  }

  function upload(id, image, unit) {
    gl.activeTexture(gl.TEXTURE0 + unit)
    gl.bindTexture(gl.TEXTURE_2D, id)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image)
  }

  // The browser test counts frames: a headless page cannot inspect the pixels,
  // but it can tell that the loop ran.
  const trace = window.__transitionTrace || (window.__transitionTrace = { draws: 0 })

  function draw(program, progress) {
    trace.draws++
    gl.viewport(0, 0, SLIDE_W, SLIDE_H)
    gl.clearColor(0, 0, 0, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)
    gl.useProgram(program)
    gl.bindBuffer(gl.ARRAY_BUFFER, quad)
    gl.enableVertexAttribArray(program.position)
    gl.vertexAttribPointer(program.position, 2, gl.FLOAT, false, 0, 0)
    gl.activeTexture(gl.TEXTURE0)
    gl.bindTexture(gl.TEXTURE_2D, fromTex)
    gl.activeTexture(gl.TEXTURE1)
    gl.bindTexture(gl.TEXTURE_2D, toTex)
    gl.uniform1i(program.from, 0)
    gl.uniform1i(program.to, 1)
    gl.uniform1f(program.progress, progress)
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
  }

  // A 1920 × 1080 canvas painted from the slide, as the rasterizer supplies it.
  function textureFor(index) {
    try {
      return slideTexture(index)
    } catch {
      return null
    }
  }

  // Hides the overlay and invalidates any frame loop still in flight.
  function cancel() {
    generation++
    if (frame) cancelAnimationFrame(frame)
    frame = 0
    if (canvas) canvas.style.display = "none"
  }

  function run(fromIndex, toIndex) {
    // Asking for the slide already showing leaves any transition in progress
    // alone; a different slide cancels it and gets its own animation.
    if (fromIndex < 0 || fromIndex === toIndex) return
    cancel()
    const transition = resolveTransition(toIndex)
    if (transition.name === "none" || !setUp()) return
    const from = textureFor(fromIndex), to = textureFor(toIndex)
    if (!from || !to) return
    const program = compile(transition.name)
    if (!program) return
    try {
      upload(fromTex, from, 0)
      upload(toTex, to, 1)
      draw(program, 0)
    } catch {
      return
    }
    const token = generation
    canvas.style.display = "block"
    const start = performance.now()
    const span = transition.duration * 1000
    const step = now => {
      if (token !== generation) return
      try {
        const progress = Math.min(1, (now - start) / span)
        draw(program, progress)
        if (progress < 1) frame = requestAnimationFrame(step)
        else {
          frame = 0
          canvas.style.display = "none"
        }
      } catch {
        // A lost context ends the animation; the new slide is already showing.
        frame = 0
        canvas.style.display = "none"
      }
    }
    frame = requestAnimationFrame(step)
  }

  return { run }
})()

function show(index) {
  const previous = current
  current = Math.max(0, Math.min(slides.length - 1, index))
  slides.forEach((slide, i) => {
    slide.classList.toggle("active", i === current)
    for (const video of slide.querySelectorAll("video")) {
      video.pause()
      if (i === current && video.dataset.autoplay === "true") {
        video.currentTime = 0
        video.play().catch(() => {
          video.muted = true // Browsers refuse sound without a gesture.
          video.play().catch(() => {})
        })
      }
    }
  })
  counter.textContent = `${current + 1} / ${slides.length}`
  history.replaceState(null, "", `#${current + 1}`)
  transitions.run(previous, current)
}

function go(step) {
  show(current + step)
}

document.addEventListener("keydown", (event) => {
  if (event.ctrlKey || event.metaKey || event.altKey) return
  if (
    (event.key === " " || event.key === "Enter") &&
    event.target instanceof Element &&
    event.target.closest("button")
  ) {
    return
  }
  switch (event.key) {
    case "ArrowRight":
    case "ArrowDown":
    case "PageDown":
    case " ":
      event.preventDefault()
      go(1)
      break
    case "ArrowLeft":
    case "ArrowUp":
    case "PageUp":
      event.preventDefault()
      go(-1)
      break
    case "Home":
      event.preventDefault()
      show(0)
      break
    case "End":
      event.preventDefault()
      show(slides.length - 1)
      break
    case "f":
    case "F":
      if (document.fullscreenElement) document.exitFullscreen()
      else stage.requestFullscreen()
      break
  }
})

// A swipe also sends a click after its pointerup. Track whether the gesture
// crossed the threshold and swallow that click in the capture phase, before
// it reaches the stage or a nav button, so one gesture moves one slide.
let swiped = false
stage.addEventListener("click", (event) => {
  if (!swiped) return
  swiped = false
  event.preventDefault()
  event.stopPropagation()
}, true)

stage.addEventListener("click", (event) => {
  if (event.target instanceof Element && (event.target.closest("video") || event.target.closest("a")))
    return
  go(1)
})
prev.addEventListener("click", (event) => {
  event.stopPropagation()
  go(-1)
})
next.addEventListener("click", (event) => {
  event.stopPropagation()
  go(1)
})

let swipe = null
stage.addEventListener("pointerdown", (event) => {
  swipe = event.clientX
  swiped = false
})
stage.addEventListener("pointerup", (event) => {
  if (swipe === null) return
  const dx = event.clientX - swipe
  swipe = null
  if (Math.abs(dx) <= 48) return
  swiped = true
  go(dx < 0 ? 1 : -1)
})

window.addEventListener("hashchange", () => show(fromHash()))

show(fromHash())

// The rasterizer's contract, for both a rendered folder deck and a baked
// one-file deck: which transition enters a slide, and that slide as a texture.
window.HypeDeck = { resolveTransition, slideTexture }
