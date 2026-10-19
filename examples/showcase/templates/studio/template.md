---
font: "Adwaita Sans"
font_heading: "Adwaita Sans"
heading_weight: 800
radius: 20
margin: "64 88"
frame: Frame
color_background: "#f4f2ee"
color_foreground: "#16161a"
color_accent: "#4f46e5"
color_indigo: "#4f46e5"
color_coral: "#f97362"
color_teal: "#14b8a6"
color_amber: "#f5a524"
color_green: "#16a34a"
color_red: "#dc2626"
color_ink: "#16161a"
color_gradient_start: "#4f46e5"
color_gradient_end: "#f97362"
chart_colors: "indigo, coral, teal, amber"
---

# Studio

A light, editorial template. Every slide gets the frame: a wordmark, the page
number, and a gradient strip. Section and Profile are reusable layouts.

<Define name="Wordmark" tone="ink">
<Row gap="12" align="center" width="auto">
<Icon name="hexagon" style="square" tone="gradient" size="sm"/>
<Text font="heading" size="24" tone="{tone}">Omacom</Text>
</Row>
</Define>

<Define name="Frame">
<Place at="top-right" inset="40 56"><Wordmark/></Place>
<Place at="bottom-right" inset="36 56"><Text size="16" tone="muted">{page} / {pages}</Text></Place>
<Place at="bottom" inset="0" height="8" fill="gradient"/>
{children}
</Define>

<Define name="Section" number="" frame="none">
<Background fill="gradient" angle="135"/>
<Place at="top-right" inset="40 56"><Wordmark tone="white"/></Place>
<Column justify="center" grow gap="12">
<Text font="heading" size="140" tone="white" line="1">{number}</Text>
<Text font="heading" size="72" tone="white" line="1.05">{children}</Text>
</Column>
</Define>

<Define name="Profile" image="" role="" email="" frame="none">
<Split ratio="62:38">
<Panel pad="72 80" gap="28">
<Eyebrow>{role}</Eyebrow>
<Text font="heading" size="80" line="1">{name}</Text>
{children}
<Text size="sm" tone="muted">{email}</Text>
</Panel>
<Panel fill="gradient" angle="160" align="center" gap="32">
<Icon name="{image}" size="3xl" style="circle" tone="white" stroke="1.5"/>
{highlights}
</Panel>
</Split>
</Define>
