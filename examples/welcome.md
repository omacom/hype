---
title: Hype
font: JetBrains Mono
theme: tokyo-night
---

# Your next idea

Markdown in. A presentation out.

---

# Keep it simple

- Write in Markdown
- Drag slides to reorder
- Duplicate the good ones
- Pick an Omarchy theme

---

> A presentation is a sequence of ideas.
> Give each one room to breathe.

— Hype

---

Pragmatic\
Ergonomic\
Aesthetic\
Sovereign

---

# The source is the document

```ruby
class Presentation
  def reorder(from, to)
    slides.insert(to, slides.delete_at(from))
  end
end
```

---

# Diagrams, too

```mermaid
flowchart LR
  md[presentation.md] --> hype([Hype]) --> pdf[PDF] & pptx[PowerPoint]
```

---

# One folder

| Directory | Contents |
| :--- | :--- |
| images/ | Pictures and backgrounds |
| videos/ | Local videos |
| presentation.md | Your slides |
