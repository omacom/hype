"use strict"

const stage = document.getElementById("stage")
const slides = [ ...stage.querySelectorAll(".slide") ]
const counter = document.getElementById("counter")
const prev = stage.querySelector(".prev")
const next = stage.querySelector(".next")
let current = -1

function fromHash() {
  const n = parseInt(location.hash.slice(1), 10)
  return Number.isInteger(n) ? n - 1 : 0
}

function show(index) {
  current = Math.max(0, Math.min(slides.length - 1, index))
  slides.forEach((slide, i) => slide.classList.toggle("active", i === current))
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

stage.addEventListener("click", () => go(1))
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
})
stage.addEventListener("pointerup", (event) => {
  if (swipe === null) return
  const dx = event.clientX - swipe
  swipe = null
  if (Math.abs(dx) > 48) go(dx < 0 ? 1 : -1)
})

window.addEventListener("hashchange", () => show(fromHash()))

show(fromHash())
