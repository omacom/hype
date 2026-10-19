#pragma once
// Shared between the component engine (blocks.cpp parses and desugars, layout.cpp
// sizes and paints), its diagrams (diagrams.cpp) and icons (icons.cpp).
#include <QColor>
#include <QFont>
#include <QImage>
#include <QMap>
#include <QPainterPath>
#include <QRectF>
#include <QSharedPointer>
#include <QString>
#include <QVariantMap>
#include <QVector>
class QPainter;
class QTextDocument;

// The colors for text and marks over a given surface.
struct Ink {
    QColor fg, muted, accent, surface, line, bg;
    bool dark = false; // The surface underneath is dark.
    qreal text = 26;   // Body size that Text inherits, before the fit factor.
    Qt::Alignment align = Qt::AlignLeft;
};

struct Node {
    QString type; // Component name, or "Text" for a Markdown chunk.
    QMap<QString, QString> props;
    QString text; // Markdown for Text; the raw inner source for components.
    QVector<Node> kids;
    int line = 1;             // Slide-relative, for error messages.
    int textLine = 0;         // The slide line of the first line of `text`; 0 if it has none.
    bool templated = false;   // Expanded from a template, where empty props are allowed.
    bool textAuthored = true; // `text` was written on the slide, not in a template.
    // Layout results, in 1920×1080 slide units.
    QRectF rect;
    Ink ink;
    QSharedPointer<QTextDocument> doc;
    qreal docScale = -1;
    qreal docWidth = -1;
    QString docInk;
    qreal measuredWidth = -1, measuredHeight = 0, measuredScale = -1;
    QColor fillA, fillB; // Resolved when placed.
    qreal fillAngle = 90;
    QString prop(const QString &key, const QString &fallback = {}) const {
        const QString value = props.value(key);
        return value.isEmpty() ? fallback : value;
    }
    bool has(const QString &key) const { return !props.value(key).isEmpty(); }
    bool flag(const QString &key, bool fallback = false) const {
        if (!props.contains(key))
            return fallback;
        const QString v = props.value(key).toLower();
        return v != "false" && v != "no" && v != "0" && v != "none" && v != "off";
    }
};

struct Ctx {
    QVariantMap palette;
    QString base;
    QStringList templateDirs; // The template's folder, then those of the templates it builds on.
    qreal s = 1;              // Fit factor: type, gaps and padding scale with it.
    qreal slack = 1;          // Share of the available height content may use; less when growing.
    QPainter *p = nullptr;
    QStringList *problems = nullptr;
    mutable qreal smallest = 1e9; // Smallest text size used, in 1080p pixels.
    bool overflow = false;
    bool lightSlide = false;
    QVector<QPair<QRectF, QColor>> bands; // Background bands and their average colors.
    QColor rootBg;
    QString fontOverride; // A component's font="…", for everything inside it.
    int boldWeight = 0;   // A component's bold="…": the weight of **bold** text inside it.
    QString boldTone;     // A component's bold-tone="…": the color of **bold** text inside it.

    qreal px(qreal value) const { return value * s; }
    // Type grows less than everything else: body text fully, display type hardly at all.
    qreal type(qreal size) const {
        if (s <= 1)
            return size * s;
        return size * (1 + (s - 1) * qBound(0.0, (50 - size) / 24, 1.0));
    }
    QColor color(const QString &key) const;
    // A theme token (accent, green, muted, surface, …) or #rrggbb. Invalid if unknown.
    QColor tone(const QString &value, const Ink &ink) const;
    QString font(bool heading) const;
    // A font="…" value as a family: heading, body, a font_<name> alias, or the family itself.
    QString family(const QString &value) const;
    // Type roles a template can set: title, number and eyebrow, through font_<role> and
    // <role>_weight. Titles and numbers fall back to the heading font, eyebrows to the body.
    QString roleFont(const QString &role) const;
    int roleWeight(const QString &role) const;
    int headingWeight() const;
    qreal radius() const;
    // An image path from images/ beside the presentation, then the template's images/.
    QString asset(const QString &file) const;
    // A video path from videos/ beside the presentation.
    QString video(const QString &file) const;
    // An image from the presentation's own images/, as video posters are found for playback.
    QString deckImage(const QString &file) const;
};

// Colors
double luminance(const QColor &color);
double contrast(const QColor &a, const QColor &b);
QColor mix(const QColor &a, const QColor &b, double amount);
Ink baseInk(const Ctx &c);
Ink inkOver(const QColor &fill, const Ink &parent, const Ctx &c);
QColor seriesColor(int index, const Ink &ink, const Ctx &c, const QStringList &custom = {});
QStringList toneNames(const Ctx &c);

// Fills: a color token, two colors for a gradient, or "gradient" for the theme's.
struct Fill {
    QColor a, b;
    qreal angle = 90; // CSS degrees: 90 is left to right, 180 is top to bottom.
    bool valid() const { return a.isValid(); }
    QColor average() const { return b.isValid() ? mix(a, b, 0.5) : a; }
};
Fill parseFill(const QString &value, const Ink &ink, const Ctx &c, qreal angle = 90);
void paintFill(QPainter *p, const QPainterPath &path, const Fill &fill, const QRectF &bounds);
void paintShadow(QPainter *p, const QRectF &rect, qreal radius, const Ctx &c, qreal strength = 1);
QPainterPath roundedPath(const QRectF &rect, qreal tl, qreal tr, qreal br, qreal bl);

// Text
struct TextStyle {
    qreal size = 26; // Body size before the fit factor.
    QColor ink, accent, muted;
    Qt::Alignment align = Qt::AlignLeft;
    bool heading = false; // Use the heading font and weight throughout.
    int weight = -1;
    bool uppercase = false;
    qreal tracking = 0; // Letter spacing, in percent.
    qreal lineHeight = 1.3;
    bool italic = false;
    bool boldTone = false; // `accent` was chosen for bold text, headings included.
    QColor tableHeader;    // Table header fill; invalid uses the foreground.
    bool zebra = true;
    bool highlightLast = false;
    Fill gradient; // Two colors fill the glyphs with a gradient instead of `ink`.
};
QSharedPointer<QTextDocument> makeText(const QString &markdown, const TextStyle &style, Ctx &c);
void drawText(QTextDocument &doc, QPointF at, QPainter *p);
// A font at `size` 1080p pixels (already scaled by the fit factor).
QFont uiFont(const Ctx &c, qreal size, bool heading = false, int weight = -1);
// One line of text in a rectangle, shrunk (not below 60%) and then elided to fit.
void drawLabel(QPainter *p, const QRectF &rect, const QString &text, QFont font,
               const QColor &color, Qt::Alignment align = Qt::AlignCenter);
qreal sizeToken(const QString &value, qreal fallback);

// Icons
bool iconExists(const QString &name);
QStringList iconSuggestions(const QString &name);
void paintIcon(QPainter *p, const QString &name, const QRectF &rect, const QColor &color,
               qreal stroke = 1.75);

// Images
QImage componentImage(const QString &path, QSize pixels, bool cover);
QSize imageSize(const QString &path);
// A picture cropped to cover `rect` (or fitted inside it), clipped to `clip`.
void drawPicture(QPainter *p, const QString &path, const QRectF &rect, const QPainterPath &clip,
                 const QString &position = {}, bool grayscale = false, bool contain = false);
// A play button centered on a picture of a video.
void paintPlay(QPainter *p, const QRectF &rect, const QColor &accent, const Ctx &c);
// A circle with someone's initials, for a person without a portrait.
void paintInitials(QPainter *p, const QRectF &circle, const QString &name, const QColor &fill,
                   const Ctx &c);

// Markdown data shared by data components.
struct Table {
    QStringList header;
    QVector<QStringList> rows;
};
Table parseTable(const QString &markdown);
struct Item {
    // `- :icon: ![](picture.png) **Title** text` splits into its parts; each is optional.
    QString icon, image, title, text;
    int level = 0;
    bool textBelow = false; // The text began on the next line, under the title.
};
QVector<Item> parseItems(const QString &markdown);
double parseNumber(const QString &text, bool *ok = nullptr);
QString plainText(const QString &markdown);

// Diagrams are leaves sized and painted from their inner Markdown (diagrams.cpp).
bool isDiagram(const QString &type);
qreal measureDiagram(Node &n, qreal width, Ctx &c);
void paintDiagram(Node &n, Ctx &c);
QStringList diagramProblems(const Node &n, const Ctx &c);

// A composed slide after parsing, expansion and desugaring.
struct Composition {
    QVector<Node> flow, backgrounds, overlays;
    QString justify;    // auto, start, center, end, between
    bool bleed = false; // A single Split fills the slide edge to edge.
    QStringList errors;
};
// Lays out at c.s and reports whether everything fits.
bool layoutComposition(Composition &slide, Ctx &c);
void paintComposition(Composition &slide, Ctx &c);
