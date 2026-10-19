---
title: "Omarchy"
theme: current
font: "JetBrains Mono"
heading_weight: 800
title_weight: 700
radius: 0
margin: "96 110"
frame: Frame
chart_colors: "accent, green, yellow, magenta"
---

<!--
The Omarchy template: the desktop's own colors (theme: current follows
whichever theme Omarchy has, as it changes), JetBrains Mono, and square tiles
with an accent border, like Hyprland's windows. Every slide is framed as the
focused window, with its number as the workspace.

Components: Title, Section, Window, Terminal, Keys. Start a new slide from any
slide here.
-->

<Define name="Frame">
<Place at="top" inset="0" height="6" fill="accent"/>
<Place at="top-right" inset="34 48"><Text size="xs" tone="muted">{page} / {pages}</Text></Place>
<Place at="top-left" inset="34 48"><Text size="xs" tone="muted">{deck-title}</Text></Place>
{children}
</Define>

<Define name="Title" eyebrow="" subtitle="">
<Column justify="center" grow gap="28">
<Eyebrow tone="accent">{eyebrow}</Eyebrow>
<Text font="heading" size="5xl" line="1.05">{children}</Text>
<Text size="lg">{subtitle}</Text>
</Column>
</Define>

<Title eyebrow="Omarchy" subtitle="A presentation that wears your desktop's theme.">
Beautiful, fast, and yours.
</Title>

---

<Define name="Section" number="" subtitle="" frame="none">
<Background band="left" size="38%" fill="accent"/>
<Place at="top" inset="0" height="6" fill="accent"/>
<Row grow align="center" gap="0">
<Column width="38%" align="center"><Text font="heading" size="220" tone="background">{number}</Text></Column>
<Column grow pad="0 0 0 96" gap="18">
<Text font="heading" size="4xl" line="1.08">{children}</Text>
<Text size="lg">{subtitle}</Text>
</Column>
</Row>
</Define>

<Section number="01" subtitle="Where we start">
The setup
</Section>

---

<Define name="Window" title="" icon="" justify="start">
<Card title="{title}" icon="{icon}" icon-style="plain" border="accent" radius="0" justify="{justify}" grow>
{children}
</Card>
</Define>

# Everything in its own tile

<Row gap="20" align="stretch" grow>
<Window title="editor" icon="code">
Write the deck in Markdown: one file, every slide.
</Window>
<Column gap="20" grow>
<Window title="preview" icon="presentation">
Hype draws each slide as you type.
</Window>
<Window title="agent" icon="bot">
An agent builds and checks slides with the same files.
</Window>
</Column>
</Row>

---

<Define name="Terminal" title="terminal">
<Card title="{title}" icon="terminal" icon-style="plain" border="accent" radius="0" fill="background" grow>
{children}
</Card>
</Define>

# Start a deck in one line

<Terminal title="~/talks">

```sh
hype new talk/presentation.md --template omarchy
hype check talk/presentation.md
hype export talk/presentation.md talk.pdf
```

</Terminal>

---

<Define name="Keys" label="">
<Row gap="20" align="center">
<Pill tone="accent" size="lg">{children}</Pill>
<Text size="lg">{label}</Text>
</Row>
</Define>

# The keys that matter

<Column gap="26">
<Keys label="Present">Ctrl + Space</Keys>
<Keys label="Overview of every slide">Ctrl + M</Keys>
<Keys label="Markdown source">Ctrl + .</Keys>
<Keys label="New slide">Ctrl + Enter</Keys>
</Column>

---

# By the numbers

<Row gap="20" align="stretch">
<Window title="themes" justify="center">
<Stat value="20+" label="Desktop themes it follows"/>
</Window>
<Window title="files" justify="center">
<Stat value="1" label="Markdown file per deck"/>
</Window>
<Window title="exports" justify="center">
<Stat value="2" label="PDF and PowerPoint"/>
</Window>
</Row>

---

<Title eyebrow="Thank you" subtitle="github.com/you · you@example.com">
Questions?
</Title>
