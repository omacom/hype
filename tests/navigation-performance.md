# Slide navigation performance investigation

Date: 2026-10-03. Base: upstream/master, f9eefda (v0.4.3).
Branch: optimize-slide-changing-speed.

## Reproduce

```sh
HYPE_NAV_BENCHMARK=1 bin/test benchmarkNavigation
```

This repository's local Qt installation can instead be used with
`HYPE_NAV_BENCHMARK=1 build/hype-env bin/test benchmarkNavigation`.
The GUI needs working local Qt multimedia initialization, even with the offscreen
platform used by `bin/test`. Run benchmarks sequentially, without a build or other
benchmark running at the same time.

The fixture `tests/fixtures/navigation.md` is an 80-slide, self-contained deck
with lists, formatted text, notes, and syntax-highlighted code. `hype check`
reports 80 slides and no problems. It can also be opened normally to reproduce
navigation by hand. No downloaded trial decks or media assets are required.

Optional environment variables:

- `HYPE_NAV_SCENARIO=markdown-cursor`, `markdown-sidebar`, or `visual-sidebar`
  selects one scenario.
- `HYPE_NAV_DECK=/absolute/path/to/presentation.md` substitutes a deck. The
  cursor scenario expects normal newline-separated slide separators.

Each scenario sends 120 real Qt Up/Down key events: 60 slides forward, then
backward. For Markdown cursor navigation, the cursor is positioned immediately
beside a separator before the timed key. The test asserts that each key selects
the expected slide and that the document is unchanged. A 70 ms pause before each
key lets the existing 60 ms prefetch timer run; the traversal also outlasts the
one-second metadata caches. Forward and reverse samples include both first
visits and revisits, rather than claiming to be strictly cold-cache measurements.

`key-ms` measures synchronous key handling. `frame-ms` measures from the key to
the next Qt Quick frame swap, including deferred UI work. It is **not** a measure
of physical display latency or of waiting for a full-resolution preview to finish.
Offscreen scheduling and background desktop load affect frame times. Results
include median, p95, maximum, mean, and the number exceeding a 16.67 ms frame
budget. Timing is opt-in, not a hardware-dependent CI threshold.

## Findings and alternatives

1. **Font picker: dominant bottleneck.** Every slide selection emitted the same
   `Deck::changed` signal used for the deck's font. `visibleFonts` consequently
   rebuilt its model and called `deck.fontName` inside the filter for Noto
   families. Each getter parsed the front matter with a new regular expression.
   This machine has 2,058 fontconfig family entries; the isolated font-picker
   change reduced median Markdown key handling from 190.8 to 4.1 ms.
   A QML `selectedFont` value now stops unchanged font values from invalidating
   that filter, including during ordinary text edits.
2. **Overbroad invalidation: secondary bottleneck.** Navigation also invalidated
   the deck size, theme, title, revision, thumbnail render identities and other
   document properties. The one-second size/render caches limited some work but
   did not prevent bindings from rerunning. Separating `selectionChanged` from
   `changed` reduced the remaining key-handler median and tail. Document changes
   still emit both, so edits, saves, reloads, theme changes and undo update the UI.
   Both renderer listeners now follow selected-slide changes explicitly. The
   final version keeps the inexpensive size-label notification and refreshes
   the newly selected and previously selected thumbnails, preserving navigation
   refresh of media metadata without rechecking the whole visible thumbnail cache.
3. **Full-document editor synchronization.** The old selection handler fetched
   the entire TextArea text to compare it with an unchanged source on every slide
   boundary. Source synchronization now runs only for document notifications;
   the selected slide editor continues updating immediately. This removes work
   proportional to document length from selection-only navigation.
4. **Hidden previews.** Tested clearing both stage image sources while the stage
   was hidden, on top of the signal split. The small key-handler change did not
   consistently improve frame times; Markdown sidebar median frame time rose
   from 8.08 to 9.07 ms in this experiment. Rejected: unnecessary mode-switch
   reloads and additional behavior changes without a clear latency benefit.
5. **Other possible work, deliberately deferred.** Pause neighbor prefetch in
   Markdown mode, lazily synchronize the hidden slide editor, cache parsed header
   scalars, or binary-search slide boundaries. The first two need careful mode
   transition/media tests; scalar caching treats a symptom now avoided at the
   binding level. The linear boundary scan was not responsible for the measured
   ~200 ms stall. Larger image/video-heavy decks may justify separate profiling
   of decoding, worker queues and GUI-thread metadata probes. No worker pool,
   cache size, playback, or prefetch policy was changed here.

## Measured comparisons

Measurements use release/O2 builds, Qt 6.9.3 on Linux Mint 22.3, a 1280×800
window, and Qt's offscreen platform. The same fixture and benchmark were used
for all variants. Experiment executables were retained separately; no timing
run overlapped another benchmark or compilation.

The isolated experiments below show **Markdown cursor navigation**, in ms.
A/B/C were each measured in one complete process run. The final implementation
was measured twice after preserving the metadata refresh behavior.

| Variant | Key median | Key p95 | Frame median | Frame p95 |
| --- | ---: | ---: | ---: | ---: |
| Baseline (complete rerun) | 192.877 | 208.466 | 198.684 | 214.687 |
| A: font picker only | 4.083 | 10.764 | 9.976 | 15.627 |
| B: selection signal only | 2.669 | 5.522 | 9.778 | 12.335 |
| C: B plus hidden preview suppression (rejected) | 2.148 | 4.742 | 10.053 | 12.776 |
| Final: metadata compatibility retained, run 1 | 2.313 | 5.491 | 10.419 | 13.043 |
| Final: metadata compatibility retained, run 2 | 2.330 | 5.185 | 10.012 | 13.129 |

Full baseline versus the **final two runs** (ranges show the two run medians,
not confidence intervals):

| Scenario | Baseline key median | Final key median | Baseline frame median | Final frame median |
| --- | ---: | ---: | ---: | ---: |
| markdown-cursor | 192.877 | 2.313–2.330 | 198.684 | 10.012–10.419 |
| markdown-sidebar | 196.508 | 2.342–2.363 | 202.099 | 8.216–8.603 |
| visual-sidebar | 197.557 | 2.047–2.431 | 202.861 | 9.659–10.636 |

Markdown boundary key handling is about **83× faster (98.8% less blocking)**;
time to the next frame is about **19–20× faster** on this machine. All 360
baseline keys and frames exceeded 16.67 ms. Across 720 transitions in the final
two runs, zero key handlers and one frame exceeded that budget (a 21.237 ms
Visual-mode frame). These are observed results, not a guarantee for every deck,
font installation, graphics backend or desktop load.

The first baseline attempt used a 90-second process limit and finished only the
two Markdown scenarios: cursor median 190.779 ms and sidebar median 196.934 ms.
It was excluded from the complete-run tables and rerun with a 180-second limit.
The initial sandboxed GUI run stalled during multimedia initialization; all
reported timings come from successful offscreen runs with local multimedia
access. The two combined-candidate runs before the metadata compatibility
adjustment are also retained in the raw results.

See [all measurements](navigation-benchmark-results.txt) for every scenario,
including maxima, means and budget exceedances. Transient executables and full
logs are under `build/navigation-experiments/` (ignored build artifacts).


## Chosen implementation and regression coverage

Keep the font-picker value, split selection notifications, and synchronize full
source only for document changes. This handles the cause without reducing image
quality or delaying the selected slide behind a debounce timer.

`navigationKeepsDocumentControlsStable` checks that selection/range changes
update the slide editor without rebuilding the font model or replacing the
source document. It also verifies editing, undo/redo, font selection, saving,
Markdown selection and returning to Visual mode. Existing GUI tests cover the
sidebar, overview, formatting, animated images and rapid image navigation.

Validation on the final code:

- Application and Qt test executable built successfully with the local Qt 6.9.3
  toolchain (`build/hype-env bin/build` and the test project).
- Fixture check: 80 slides, no problems.
- Two complete final benchmark runs: 3 passed, 0 failed per run; 720 total
  keyboard transitions, including the unchanged-document assertions.
- Focused GUI regression run: 8 passed, 0 failed.
- Full Qt suite: **64 passed, 1 failed, 5 skipped**. The failure is
  `pdfPreserves4kDetailAndReusesImages`: at x=100 the PDF renderer produces
  `#670067` where the test expects `#0000ff`. Running that same test in the
  preserved upstream-baseline executable reproduces the exact same failure.
  It is unrelated to these navigation changes and has not been hidden or fixed
  as part of this work. Full logs: `build/navigation-experiments/full-suite.log`
  and `baseline-pdf.log`.
- Skips: desktop portal opt-in, unavailable Tokyo Night theme, and three opt-in
  performance tests (the new navigation benchmark was run separately).
- `git diff --check` passed. The pre-existing `examples/welcome.md` edits and
  generated files were preserved.

