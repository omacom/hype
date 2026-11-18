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
  const match = new RegExp("<!--\\s*hype:[\\s\\S]*?\\b" + key + '="([^"]*)"').exec(source)
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
  parseFlags(media, match[1].trim())
  if (media.position) {
    // A placed image leaves the slide's own layout and colors alone.
    media.span = false
    media.overlay = 0
    if (!media.size) media.size = 10
    return media
  }
  media.overlay = (!media.video || media.span) && media.text.trim() ? 0.25 : 0
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
  if (!first || first.index !== 0) return
  if (!["fit", "span", "loop", "muted"].includes(first[1]) && first[2] === undefined) return
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
  if (explicitOverlay !== "") {
    const opacity = parseFloat(explicitOverlay)
    if (!Number.isNaN(opacity) && opacity >= 0 && opacity <= 1) media.overlay = opacity
  }
}

// --- Deck rendering --------------------------------------------------------

function renderMarkdownDeck() {
  const sourceBlock = document.getElementById("source")
  if (!sourceBlock) return
  // The one-file export embeds its Markdown the same way, beside slides that are
  // already pictures. There the block is the editable source for anyone who
  // receives the file, not a second deck to render on top of the first.
  if (stage.querySelector("section.slide")) return
  const source = sourceBlock.textContent.replace(/<\\\//g, "</").trim() + "\n"
  const paletteTag = document.getElementById("palette")
  const palette = paletteTag ? JSON.parse(paletteTag.textContent) : {}
  const parsed = parseDeck(source)
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
  if (media.svg && !media.file) {
    drawSvg(canvas, text ? BELOW_AREA : FILL_AREA, media.svg, palette)
    if (text) area = BAND_AREA
  }
  if (media.file && drawMedia(canvas, media, text, palette, COLOR_RE.test(fgProperty)))
    area = BAND_AREA
  if (text) drawText(canvas, area, source, media, palette, fits)
}

function drawSvg(canvas, rect, svg, palette) {
  const box = document.createElement("div")
  box.className = "svg-box"
  placeAt(box, rect)
  box.innerHTML = themedSvg(svg, palette)
  const drawing = box.firstChild
  if (drawing) {
    drawing.style.maxWidth = "100%"
    drawing.style.maxHeight = "100%"
  }
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
  const rect = media.span ? { left: 0, top: 0, width: SLIDE_W, height: SLIDE_H }
    : placed ? placedRect(media)
    : !media.video || !text ? FILL_AREA : BELOW_AREA
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
  content.style.color = palette.foreground
  buildBlocks(content, media.text)
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
}

function buildBlocks(content, text) {
  const lines = text.split("\n")
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
      if (language.toLowerCase() !== "svg") {
        const pre = document.createElement("pre")
        const codeElement = document.createElement("code")
        codeElement.innerHTML = highlight(body, language)
        pre.appendChild(codeElement)
        content.appendChild(pre)
      }
      i = j >= lines.length ? lines.length : j + 1
      continue
    }
    const heading = /^ {0,3}(#{1,6}) +(.*)$/.exec(line)
    if (heading) {
      const element = document.createElement("h" + heading[1].length)
      inline(element, heading[2])
      content.appendChild(element)
      i++
      continue
    }
    if (line.trimStart().startsWith(">")) {
      const quoteElement = document.createElement("blockquote")
      while (i < lines.length && lines[i].trimStart().startsWith(">")) {
        const body = lines[i].trimStart().replace(/^>\s?/, "")
        const paragraph = document.createElement("p")
        if (body.startsWith("—")) paragraph.className = "attribution"
        inline(paragraph, body)
        quoteElement.appendChild(paragraph)
        i++
      }
      content.appendChild(quoteElement)
      continue
    }
    const item = /^ {0,3}([-*+]|\d+[.)]) +(.*)$/.exec(line)
    if (item) {
      // The marker decides the list kind, never the text after it.
      const ordered = /\d/.test(item[1])
      const listElement = document.createElement(ordered ? "ol" : "ul")
      while (i < lines.length) {
        const entry = /^ {0,3}([-*+]|\d+[.)]) +(.*)$/.exec(lines[i])
        if (!entry) break
        const li = document.createElement("li")
        inline(li, entry[2])
        listElement.appendChild(li)
        i++
      }
      content.appendChild(listElement)
      continue
    }
    if (line.includes("|") && i + 1 < lines.length && TABLE_DELIMITER.test(lines[i + 1])) {
      content.appendChild(buildTable(lines, i))
      while (i < lines.length && lines[i].includes("|") && lines[i].trim()) i++
      continue
    }
    const paragraph = document.createElement("p")
    const body = []
    while (i < lines.length && lines[i].trim() && !lineIsFence(lines[i]) &&
           !/^ {0,3}#{1,6} /.test(lines[i]) && !lines[i].trimStart().startsWith(">") &&
           !/^ {0,3}([-*+]|\d+[.)]) /.test(lines[i]) && !lines[i].includes("|"))
      body.push(lines[i++])
    inline(paragraph, body.join("\n"))
    content.appendChild(paragraph)
  }
}

function buildTable(lines, start) {
  const cells = line => {
    let text = line.trim()
    if (text.startsWith("|")) text = text.slice(1)
    if (text.endsWith("|")) text = text.slice(0, -1)
    return text.split(/(?<!\\)\|/)
  }
  const alignment = cells(lines[start + 1]).map(cell => {
    const marker = cell.trim()
    return marker.startsWith(":") && marker.endsWith(":") ? "center" : marker.endsWith(":") ? "right" : "left"
  })
  const table = document.createElement("table")
  const head = document.createElement("thead")
  const headRow = document.createElement("tr")
  cells(lines[start]).forEach((cell, column) => {
    const th = document.createElement("th")
    th.style.textAlign = alignment[column] || "left"
    inline(th, padCell(cell))
    headRow.appendChild(th)
  })
  head.appendChild(headRow)
  table.appendChild(head)
  const body = document.createElement("tbody")
  for (let i = start + 2; i < lines.length && lines[i].includes("|") && lines[i].trim(); i++) {
    const row = document.createElement("tr")
    cells(lines[i]).forEach((cell, column) => {
      const td = document.createElement("td")
      td.style.textAlign = alignment[column] || "left"
      inline(td, padCell(cell))
      row.appendChild(td)
    })
    body.appendChild(row)
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

function inline(parent, text) {
  const pattern = new RegExp(INLINE_SOURCE, "g")
  let last = 0
  let match
  while ((match = pattern.exec(text))) {
    if (match.index > last) parent.appendChild(document.createTextNode(text.slice(last, match.index)))
    const [, ticks, , bold, boldUnderscore, italic, underline, linkText, linkTarget] = match
    if (ticks) {
      const element = document.createElement("code")
      element.textContent = match[0].slice(ticks.length, -ticks.length)
      parent.appendChild(element)
    } else if (bold !== undefined || boldUnderscore !== undefined) {
      const element = document.createElement("strong")
      inline(element, bold ?? boldUnderscore)
      parent.appendChild(element)
    } else if (italic !== undefined) {
      const element = document.createElement("em")
      inline(element, italic)
      parent.appendChild(element)
    } else if (underline !== undefined) {
      const element = document.createElement("u")
      inline(element, underline)
      parent.appendChild(element)
    } else if (linkText !== undefined) {
      const element = document.createElement("a")
      if (/^(https?:|mailto:|#|\.{0,2}\/|[a-z0-9._-]+\.md$)/i.test(linkTarget)) element.href = linkTarget
      element.textContent = linkText
      parent.appendChild(element)
    }
    last = match.index + match[0].length
  }
  if (last < text.length) parent.appendChild(document.createTextNode(text.slice(last)))
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

function highlight(code, language) {
  language = (language || "").toLowerCase().split(" ")[0]
  const hash = ["sh", "bash", "shell", "zsh", "python", "py", "ruby", "rb", "yaml", "yml", "toml", "ini", "conf", "r", "perl", "makefile", "dockerfile"].includes(language)
  const dash = ["sql", "lua", "haskell", "hs", "elm", "ada"].includes(language)
  const words = new Set(KEYWORDS.common.split(" ").concat(KEYWORDS.javascript.split(" ")))
  for (const [languages, list] of [[["python", "py"], "python"], [["ruby", "rb"], "ruby"],
                                   [["rust", "rs"], "rust"], [["go", "golang"], "go"],
                                   [["sh", "bash", "shell", "zsh"], "shell"], [["sql"], "sql"]])
    if (languages.includes(language)) KEYWORDS[list].split(" ").forEach(word => words.add(word))
  const span = (cls, text) => `<span class="${cls}">${escapeHtml(text)}</span>`
  const patterns = []
  if (hash) patterns.push([/#[^\n]*/y, "c"])
  if (dash) patterns.push([/--[^\n]*/y, "c"])
  if (!dash) patterns.push([/\/\/[^\n]*/y, "c"])
  patterns.push([/\/\*[\s\S]*?\*\//y, "c"])
  patterns.push([/"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`/y, "s"])
  patterns.push([/\b\d[\w.]*\b/y, "n"])
  patterns.push([/[A-Za-z_$][\w$]*/y, null])
  let result = "", plain = ""
  const flush = () => { if (plain) { result += escapeHtml(plain); plain = "" } }
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
        if (words.has(word)) result += span("k", word)
        else if (/^[A-Z]/.test(word)) result += span("t", word)
        else if (/^\s*\(/.test(code.slice(i + word.length))) result += span("f", word)
        else result += escapeHtml(word)
      } else result += span(cls, match[0])
      i += match[0].length
      break
    }
    if (!matched) plain += code[i++]
  }
  flush()
  return result
}

function escapeHtml(text) {
  return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

// --- Wiring ----------------------------------------------------------------

renderMarkdownDeck()

const slides = [...stage.querySelectorAll(".slide")]

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

function show(index) {
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
