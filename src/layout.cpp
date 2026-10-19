// Components: theme tokens, text, and the layout engine that sizes, places and
// paints the primitives produced by blocks.cpp.
#include "blocks_p.h"
#include "renderer.h"
#include "fonts.h"
#include "syntax.h"
#include "images.h"
#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QDir>
#include <QFileInfo>
#include <QGlyphRun>
#include <QImageReader>
#include <QLinearGradient>
#include <QMutex>
#include <QPaintEngine>
#include <QPainter>
#include <QRawFont>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>
#include <cmath>

// ---------------------------------------------------------------------------
// Theme tokens

QColor Ctx::color(const QString &key) const {
    const QColor value(palette.value(key).toString());
    if (value.isValid() && palette.value(key).toString().startsWith('#')) return value;
    const QColor bg(palette.value("background", "#1a1b26").toString());
    const QColor fg(palette.value("foreground", "#c0caf5").toString());
    if (key == "surface")
        return luminance(bg) > 0.5 ? (luminance(bg) > 0.96 ? mix(bg, fg, 0.035) : QColor("#ffffff"))
                                   : mix(bg, fg, 0.075);
    if (key == "muted") return mix(fg, bg, 0.38);
    if (key == "line") return mix(bg, fg, 0.15);
    if (key == "gradient_start") return color("accent");
    if (key == "gradient_end") return color("magenta");
    if (key == "accent") return QColor("#7aa2f7");
    if (key == "background") return bg;
    if (key == "foreground") return fg;
    return {};
}

QColor Ctx::tone(const QString &raw, const Ink &ink) const {
    const QString v = raw.trimmed().toLower();
    if (v.isEmpty()) return {};
    if (v.startsWith('#')) return QColor(v);
    if (v == "ink" || v == "text" || v == "foreground") return ink.fg;
    if (v == "muted") return ink.muted;
    if (v == "accent") return ink.accent;
    if (v == "line" || v == "border") return ink.line;
    if (v == "surface") return color("surface");
    if (v == "background" || v == "bg") return color("background");
    if (v == "white") return Qt::white;
    if (v == "gradient") return mix(color("gradient_start"), color("gradient_end"), 0.5);
    if (v == "black") return Qt::black;
    if (palette.contains(v) && QColor(palette.value(v).toString()).isValid() &&
        palette.value(v).toString().startsWith('#'))
        return QColor(palette.value(v).toString());
    if (const QColor derived = color(v); derived.isValid()) return derived;
    return {};
}

QStringList toneNames(const Ctx &c) {
    QStringList names{"accent", "ink", "muted", "surface", "background", "line", "gradient"};
    for (auto it = c.palette.cbegin(); it != c.palette.cend(); ++it) {
        const QString value = it.value().toString();
        if (!value.startsWith('#') || names.contains(it.key()) || it.key() == "foreground" ||
            it.key().startsWith("gradient"))
            continue;
        names << it.key();
    }
    return names;
}

QString Ctx::font(bool heading) const {
    if (!fontOverride.isEmpty()) return fontOverride;
    const QString body = palette.value("font", "JetBrains Mono").toString();
    return heading ? palette.value("font_heading", body).toString() : body;
}
QString Ctx::family(const QString &value) const {
    const QString v = value.trimmed();
    const QString body = palette.value("font", "JetBrains Mono").toString();
    if (v == "heading") return palette.value("font_heading", body).toString();
    if (v == "body") return body;
    if (v == "title" || v == "number" || v == "eyebrow") return roleFont(v);
    const QString alias = palette.value("font_" + v.toLower()).toString();
    return alias.isEmpty() ? v : alias;
}
QString Ctx::roleFont(const QString &role) const {
    const QString alias = palette.value("font_" + role).toString();
    if (!alias.isEmpty()) return alias;
    const QString body = palette.value("font", "JetBrains Mono").toString();
    return role == "body" || role == "eyebrow" ? body : palette.value("font_heading", body).toString();
}
int Ctx::roleWeight(const QString &role) const {
    bool ok = false;
    const int weight = palette.value(role + "_weight").toInt(&ok);
    if (ok) return qBound(100, weight, 950);
    return role == "eyebrow" ? 700 : role == "body" ? 400 : headingWeight();
}
int Ctx::headingWeight() const { return qBound(100, palette.value("heading_weight", 700).toInt(), 950); }
qreal Ctx::radius() const {
    bool ok = false;
    const qreal r = palette.value("radius").toString().toDouble(&ok);
    return ok ? r : 18;
}
QString Ctx::asset(const QString &file) const {
    if (file.isEmpty() || QFileInfo(file).isAbsolute()) return file;
    const QString relative = file.startsWith("images/") ? file : "images/" + file;
    const QString own = QDir(base).filePath(relative);
    if (QFileInfo::exists(own)) return own;
    for (const QString &dir : templateDirs)
        if (const QString shared = QDir(dir).filePath(relative); QFileInfo::exists(shared)) return shared;
    return own;
}
QString Ctx::deckImage(const QString &file) const {
    if (file.isEmpty() || QFileInfo(file).isAbsolute()) return file;
    return QDir(base).filePath(file.startsWith("images/") ? file : "images/" + file);
}
QString Ctx::video(const QString &file) const {
    if (file.isEmpty() || QFileInfo(file).isAbsolute()) return file;
    return QDir(base).filePath(file.startsWith("videos/") ? file : "videos/" + file);
}

// ---------------------------------------------------------------------------
// Colors

double luminance(const QColor &color) {
    auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}
double contrast(const QColor &a, const QColor &b) {
    const double x = luminance(a), y = luminance(b);
    return (qMax(x, y) + 0.05) / (qMin(x, y) + 0.05);
}
QColor mix(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

Ink baseInk(const Ctx &c) {
    Ink ink;
    ink.bg = c.color("background");
    ink.fg = c.color("foreground");
    ink.accent = c.color("accent");
    ink.surface = c.color("surface");
    ink.muted = c.color("muted");
    ink.line = c.color("line");
    ink.dark = luminance(ink.bg) < 0.4;
    return ink;
}

Ink inkOver(const QColor &fill, const Ink &parent, const Ctx &c) {
    Ink ink = parent;
    ink.bg = fill;
    ink.dark = luminance(fill) < 0.4;
    // Prefer the colors already in play; fall back to white or near black.
    const QColor candidates[] = {parent.fg, c.color("foreground"), c.color("background"), QColor("#ffffff"),
                                 QColor("#161616")};
    QColor best = candidates[0];
    for (const QColor &candidate : candidates) {
        if (contrast(candidate, fill) >= 4.5) {
            best = candidate;
            break;
        }
        if (contrast(candidate, fill) > contrast(best, fill)) best = candidate;
    }
    // Brand colors (saturated reds, pinks, blues, purples) carry white, as slide text on
    // them is large or bold, where 3:1 is enough.
    const QColor hsl = fill.toHsl();
    if (hsl.hslSaturationF() > 0.45 && hsl.lightnessF() > 0.25 && hsl.lightnessF() < 0.7 &&
        contrast(QColor("#ffffff"), fill) >= 3)
        best = QColor("#ffffff");
    ink.fg = best;
    ink.accent = contrast(c.color("accent"), fill) >= 3 ? c.color("accent") : best;
    ink.muted = mix(best, fill, 0.3);
    ink.line = mix(fill, best, 0.18);
    return ink;
}

QColor seriesColor(int index, const Ink &ink, const Ctx &c, const QStringList &custom) {
    QStringList order = custom;
    if (order.isEmpty())
        order = c.palette.value("chart_colors").toString().split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
    if (order.isEmpty()) order = {"accent", "magenta", "cyan", "yellow", "green", "red"};
    // Never cycle hues: past the palette, series fold into a neutral.
    if (index >= order.size()) return ink.muted;
    const QColor color = c.tone(order[index], ink);
    return color.isValid() ? color : ink.muted;
}

Fill parseFill(const QString &raw, const Ink &ink, const Ctx &c, qreal angle) {
    Fill fill;
    fill.angle = angle;
    const QString value = raw.trimmed().toLower();
    if (value.isEmpty() || value == "none" || value == "transparent") return fill;
    if (value == "gradient") {
        fill.a = c.color("gradient_start");
        fill.b = c.color("gradient_end");
        if (c.palette.contains("gradient_angle") && angle == 90) fill.angle = c.palette.value("gradient_angle").toDouble();
        return fill;
    }
    const QStringList parts = value.split(QRegularExpression("\\s*(?:,|\\s+to\\s+|\\s+|>)\\s*"), Qt::SkipEmptyParts);
    if (parts.size() == 2) {
        fill.a = c.tone(parts[0], ink);
        fill.b = c.tone(parts[1], ink);
        if (!fill.a.isValid() || !fill.b.isValid()) return Fill{};
        return fill;
    }
    fill.a = c.tone(value, ink);
    return fill;
}

void paintFill(QPainter *p, const QPainterPath &path, const Fill &fill, const QRectF &bounds) {
    if (!fill.valid()) return;
    if (!fill.b.isValid()) {
        p->fillPath(path, fill.a);
        return;
    }
    const double radians = qDegreesToRadians(fill.angle);
    const QPointF direction(std::sin(radians), -std::cos(radians));
    const double length = std::abs(bounds.width() * direction.x()) + std::abs(bounds.height() * direction.y());
    QLinearGradient gradient(bounds.center() - direction * length / 2, bounds.center() + direction * length / 2);
    gradient.setColorAt(0, fill.a);
    gradient.setColorAt(1, fill.b);
    p->fillPath(path, gradient);
}

QPainterPath roundedPath(const QRectF &r, qreal tl, qreal tr, qreal br, qreal bl) {
    const qreal limit = qMin(r.width(), r.height()) / 2;
    tl = qMin(tl, limit), tr = qMin(tr, limit), br = qMin(br, limit), bl = qMin(bl, limit);
    QPainterPath path;
    path.moveTo(r.left() + tl, r.top());
    path.lineTo(r.right() - tr, r.top());
    if (tr > 0) path.arcTo(QRectF(r.right() - 2 * tr, r.top(), 2 * tr, 2 * tr), 90, -90);
    path.lineTo(r.right(), r.bottom() - br);
    if (br > 0) path.arcTo(QRectF(r.right() - 2 * br, r.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
    path.lineTo(r.left() + bl, r.bottom());
    if (bl > 0) path.arcTo(QRectF(r.left(), r.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
    path.lineTo(r.left(), r.top() + tl);
    if (tl > 0) path.arcTo(QRectF(r.left(), r.top(), 2 * tl, 2 * tl), 180, -90);
    path.closeSubpath();
    return path;
}

void paintShadow(QPainter *p, const QRectF &rect, qreal radius, const Ctx &c, qreal strength) {
    // A soft drop shadow, blurred once per size at a quarter of slide resolution.
    static QMutex mutex;
    static QCache<QString, QImage> cache(8 * 1024);
    const qreal blur = c.px(30), offset = c.px(12), scale = 0.25;
    const QSize size(qCeil((rect.width() + 2 * blur) * scale), qCeil((rect.height() + 2 * blur) * scale));
    if (size.isEmpty()) return;
    const QString key = QString("%1x%2:%3:%4").arg(size.width()).arg(size.height()).arg(qRound(radius)).arg(qRound(blur));
    QImage shadow;
    {
        QMutexLocker lock(&mutex);
        if (const QImage *cached = cache.object(key)) shadow = *cached;
    }
    if (shadow.isNull()) {
        shadow = QImage(size, QImage::Format_ARGB32_Premultiplied);
        shadow.fill(Qt::transparent);
        QPainter sp(&shadow);
        sp.setRenderHint(QPainter::Antialiasing);
        sp.scale(scale, scale);
        sp.fillPath(roundedPath(QRectF(blur, blur, rect.width(), rect.height()), radius, radius, radius, radius),
                    Qt::black);
        sp.end();
        shadow = boxBlur(shadow, qMax(1, qRound(blur * scale / 2)));
        QMutexLocker lock(&mutex);
        cache.insert(key, new QImage(shadow), qMax(1, int(shadow.sizeInBytes() / 1024)));
    }
    p->save();
    p->setOpacity(p->opacity() * 0.11 * strength);
    p->drawImage(QRectF(rect.x() - blur, rect.y() - blur + offset, rect.width() + 2 * blur, rect.height() + 2 * blur),
                 shadow);
    p->restore();
}

// ---------------------------------------------------------------------------
// Text

qreal sizeToken(const QString &value, qreal fallback) {
    static const QHash<QString, qreal> sizes{
        {"xs", 18}, {"sm", 22}, {"md", 26}, {"lg", 32}, {"xl", 40}, {"2xl", 52}, {"3xl", 64}, {"4xl", 80},
        {"5xl", 104}, {"card", 30}, {"eyebrow", 18}, {"stat-sm", 44}, {"stat", 64}, {"stat-lg", 88},
        {"stat-xl", 120}};
    const QString v = value.trimmed().toLower();
    if (v.isEmpty()) return fallback;
    if (sizes.contains(v)) return sizes.value(v);
    bool ok = false;
    const qreal number = QString(v).remove("px").toDouble(&ok);
    return ok && number > 0 ? number : fallback;
}

static int weightToken(const QString &value, int fallback) {
    static const QHash<QString, int> weights{{"thin", 100},   {"light", 300},    {"regular", 400}, {"normal", 400},
                                             {"medium", 500}, {"semibold", 600}, {"bold", 700},    {"heavy", 800},
                                             {"black", 900}};
    const QString v = value.trimmed().toLower();
    if (v.isEmpty()) return fallback;
    if (weights.contains(v)) return weights.value(v);
    bool ok = false;
    const int number = v.toInt(&ok);
    return ok ? qBound(100, number, 950) : fallback;
}

namespace {
// font="…", bold="…" and bold-tone="…" on a component reach everything inside it, as in CSS.
struct FontScope {
    Ctx &c;
    const QString font;
    const int bold;
    const QString boldTone;
    FontScope(Ctx &ctx, const Node &n) : c(ctx), font(ctx.fontOverride), bold(ctx.boldWeight), boldTone(ctx.boldTone) {
        // @heading is a component's own choice for its titles and numbers; a font set on the
        // component itself takes precedence.
        const QString value = n.prop("font");
        if (value.startsWith('@')) {
            if (c.fontOverride.isEmpty()) c.fontOverride = c.roleFont(value.mid(1));
        } else if (!value.isEmpty())
            c.fontOverride = c.family(value);
        if (n.has("bold")) c.boldWeight = weightToken(n.prop("bold"), c.boldWeight);
        if (n.has("bold-tone")) c.boldTone = n.prop("bold-tone");
    }
    ~FontScope() {
        c.fontOverride = font;
        c.boldWeight = bold;
        c.boldTone = boldTone;
    }
};
}

QFont uiFont(const Ctx &c, qreal size, bool heading, int weight) {
    QFont font(fonts::styled(c.font(heading), weight > 0 ? weight : heading ? c.headingWeight() : 400));
    font.setPixelSize(qMax(1, qRound(size)));
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Weight(weight > 0 ? weight : heading ? c.headingWeight() : 400));
    c.smallest = qMin(c.smallest, size / qMax(0.01, c.s) * c.s);
    return font;
}

void drawLabel(QPainter *p, const QRectF &rect, const QString &text, QFont font, const QColor &color,
               Qt::Alignment align) {
    if (text.isEmpty() || rect.width() <= 1) return;
    const int original = font.pixelSize();
    QFontMetricsF metrics(font);
    while (metrics.horizontalAdvance(text) > rect.width() && font.pixelSize() > original * 0.6) {
        font.setPixelSize(font.pixelSize() - 1);
        metrics = QFontMetricsF(font);
    }
    p->setFont(font);
    p->setPen(color);
    p->drawText(rect, align | Qt::TextSingleLine, metrics.elidedText(text, Qt::ElideRight, rect.width()));
}

// Ordinary line breaks stay visible. Qt would turn a Markdown hard break into a new block,
// with paragraph spacing; a line separator keeps the lines of a paragraph together.
static QString keepLines(const QString &markdown) {
    const QStringList lines = markdown.split('\n');
    const QStringList visible = outsideCode(markdown, false).split('\n');
    static const QRegularExpression starts("^\\s*(#|[-*+]\\s|\\d+[.)]\\s|\\||>|```|~~~|<!--)");
    QString out;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        const bool code = visible.value(i).trimmed().isEmpty() && !line.trimmed().isEmpty();
        const bool joins = i + 1 < lines.size() && !code && !line.trimmed().isEmpty() &&
                           !lines[i + 1].trimmed().isEmpty() && !starts.match(lines[i + 1]).hasMatch() &&
                           !line.trimmed().startsWith('|') && !line.trimmed().startsWith('#') &&
                           !visible.value(i + 1).trimmed().isEmpty();
        if (joins) {
            while (line.endsWith(' ')) line.chop(1);
            if (line.endsWith('\\')) line.chop(1);
            out += line + QChar(0x2028);
            continue;
        }
        out += line + (i + 1 < lines.size() ? "\n" : "");
    }
    return out;
}

QSharedPointer<QTextDocument> makeText(const QString &markdown, const TextStyle &st, Ctx &c) {
    auto doc = QSharedPointer<QTextDocument>::create();
    doc->setUndoRedoEnabled(false);
    doc->setMarkdown(keepLines(markdown), QTextDocument::MarkdownDialectGitHub);
    doc->setPageSize(QSizeF(0, 0));
    doc->setDocumentMargin(0);
    const qreal size = c.type(st.size);
    const qreal k = st.size / 26.0;
    const QString body = c.font(st.heading), heading = c.font(true);
    QFont base(body);
    base.setPixelSize(qMax(1, qRound(size)));
    base.setHintingPreference(QFont::PreferNoHinting);
    doc->setDefaultFont(base);
    doc->setIndentWidth(size * 1.2);
    QTextOption option;
    option.setUseDesignMetrics(true);
    option.setWrapMode(QTextOption::WordWrap);
    option.setAlignment(st.align);
    doc->setDefaultTextOption(option);
    const qreal headingSizes[] = {0, 60, 42, 30, 25, 22, 20};
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat bf = block.blockFormat();
        const int level = qBound(0, bf.headingLevel(), 6);
        const qreal blockSize = level ? c.type(qMax(st.size, headingSizes[level] * k)) : size;
        const bool headingFont = st.heading || (level >= 1 && level <= 3);
        bf.setAlignment(st.align);
        bf.setLineHeight((level && level <= 2 ? 108 : st.lineHeight * 100), QTextBlockFormat::ProportionalHeight);
        bf.setTopMargin(level >= 3 ? blockSize * 0.15 : 0);
        bf.setBottomMargin(block.textList() ? size * 0.28 : level ? blockSize * 0.32 : size * 0.55);
        if (!block.next().isValid()) bf.setBottomMargin(0);
        cursor.setBlockFormat(bf);
        c.smallest = qMin(c.smallest, blockSize);
        // Formatting can merge neighboring fragments, so collect them before changing any.
        QVector<QTextFragment> fragments;
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().isValid()) fragments << it.fragment();
        for (const QTextFragment &fragment : fragments) {
            QTextCursor text(doc.data());
            text.setPosition(fragment.position());
            text.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            const QTextCharFormat existing = fragment.charFormat();
            QTextCharFormat cf;
            const bool code = existing.fontFixedPitch() || existing.fontFamilies().toStringList().contains("monospace");
            cf.setFontFamilies({code ? QString("JetBrains Mono") : headingFont ? heading : body});
            cf.setProperty(QTextFormat::FontPixelSize, qMax(1, qRound(blockSize)));
            // Qt marks a heading's own text bold, with a size adjustment that **bold** words lack.
            const bool strong = existing.fontWeight() >= QFont::Bold &&
                                !(level && existing.hasProperty(QTextFormat::FontSizeAdjustment));
            int weight = st.weight > 0 ? st.weight : headingFont ? c.headingWeight() : level ? 700 : 400;
            if (strong && !headingFont) weight = c.boldWeight > 0 ? c.boldWeight : qMax(weight, 700);
            cf.setFontWeight(weight);
            if (!code) cf.setFontFamilies({fonts::styled(headingFont ? heading : body, weight)});
            QColor color = st.ink;
            if (strong && (!level || st.boldTone) && st.accent.isValid()) color = st.accent;
            if (st.gradient.b.isValid()) {
                // Each line runs from one color to the other across its own width.
                const double radians = qDegreesToRadians(st.gradient.angle);
                const QPointF direction(std::sin(radians) / 2, -std::cos(radians) / 2);
                QLinearGradient gradient(QPointF(0.5, 0.5) - direction, QPointF(0.5, 0.5) + direction);
                gradient.setCoordinateMode(QGradient::ObjectMode);
                gradient.setColorAt(0, st.gradient.a);
                gradient.setColorAt(1, st.gradient.b);
                cf.setForeground(QBrush(gradient));
            } else
                cf.setForeground(color);
            if (st.italic) cf.setFontItalic(true);
            if (st.uppercase) cf.setFontCapitalization(QFont::AllUppercase);
            if (st.tracking) cf.setFontLetterSpacing(100 + st.tracking);
            cf.setFontLetterSpacingType(QFont::PercentageSpacing);
            // Qt marks headings with a relative size that would override the pixel size.
            QTextCharFormat merged = existing;
            merged.clearProperty(QTextFormat::FontSizeAdjustment);
            merged.clearProperty(QTextFormat::FontPointSize);
            merged.merge(cf);
            text.setCharFormat(merged);
        }
    }
    for (auto it = doc->rootFrame()->begin(); !it.atEnd(); ++it) {
        auto table = qobject_cast<QTextTable *>(it.currentFrame());
        if (!table) continue;
        auto fmt = table->format();
        fmt.setBorder(0);
        fmt.setBorderCollapse(true);
        fmt.setCellPadding(size * 0.42);
        fmt.setCellSpacing(0);
        fmt.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        table->setFormat(fmt);
        const QColor header = st.tableHeader.isValid() ? st.tableHeader : st.ink;
        const QColor headerInk = contrast(QColor("#ffffff"), header) >= contrast(QColor("#161616"), header)
                                     ? QColor("#ffffff") : QColor("#161616");
        const QColor zebra = mix(c.color("surface"), st.ink, 0.045);
        const QColor highlight = mix(c.color("surface"), c.color("yellow").isValid() ? c.color("yellow") : st.accent, 0.55);
        for (int r = 0; r < table->rows(); ++r)
            for (int col = 0; col < table->columns(); ++col) {
                QTextTableCell cell = table->cellAt(r, col);
                QTextTableCellFormat cf = cell.format().toTableCellFormat();
                const bool last = r == table->rows() - 1 && r > 0;
                QColor background = r == 0 ? header : (st.highlightLast && last) ? highlight
                                                    : (st.zebra && r % 2 == 0) ? zebra : QColor(Qt::transparent);
                cf.setBackground(background);
                cf.setBottomBorder(r == 0 ? 0 : 1);
                cf.setBottomBorderBrush(mix(c.color("surface"), st.ink, 0.12));
                cf.setBottomBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                cell.setFormat(cf);
                QTextCursor cursor = cell.firstCursorPosition();
                cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                QTextCharFormat text;
                if (r == 0) {
                    text.setForeground(headerInk);
                    text.setFontWeight(700);
                } else if (st.highlightLast && last)
                    text.setFontWeight(700);
                cursor.mergeCharFormat(text);
            }
    }
    highlightCode(*doc, c.palette);
    return doc;
}

// Qt's PDF engine paints text whose color is an object-mode gradient in plain black,
// so a PDF gets each line of gradient text as outlines filled across the line.
static bool outlineGradientText(QTextDocument &doc, QPainter *p) {
    if (!p->paintEngine() || p->paintEngine()->type() != QPaintEngine::Pdf) return false;
    const auto first = doc.begin().begin();
    if (first.atEnd()) return false;
    const QGradient *gradient = first.fragment().charFormat().foreground().gradient();
    if (!gradient || gradient->type() != QGradient::LinearGradient ||
        gradient->coordinateMode() != QGradient::ObjectMode) return false;
    const auto *linear = static_cast<const QLinearGradient *>(gradient);
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const QTextLayout *layout = block.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            QPainterPath line;
            for (const QGlyphRun &run : layout->lineAt(i).glyphRuns()) {
                const QRawFont font = run.rawFont();
                const auto glyphs = run.glyphIndexes();
                const auto positions = run.positions();
                for (int g = 0; g < glyphs.size(); ++g)
                    line.addPath(font.pathForGlyph(glyphs[g]).translated(positions[g]));
            }
            line.translate(layout->position());
            const QRectF r = line.boundingRect();
            QLinearGradient fill(r.topLeft() + QPointF(linear->start().x() * r.width(), linear->start().y() * r.height()),
                                 r.topLeft() + QPointF(linear->finalStop().x() * r.width(), linear->finalStop().y() * r.height()));
            fill.setStops(linear->stops());
            p->fillPath(line, fill);
        }
    }
    // Invisible text over the outlines keeps the words searchable and selectable.
    QScopedPointer<QTextDocument> words(doc.clone());
    QTextCursor all(words.data());
    all.select(QTextCursor::Document);
    QTextCharFormat clear;
    clear.setForeground(Qt::transparent);
    all.mergeCharFormat(clear);
    words->setTextWidth(doc.textWidth());
    words->documentLayout()->draw(p, QAbstractTextDocumentLayout::PaintContext());
    return true;
}

void drawText(QTextDocument &doc, QPointF at, QPainter *p) {
    p->save();
    p->translate(at);
    if (!outlineGradientText(doc, p)) {
        QAbstractTextDocumentLayout::PaintContext context;
        doc.documentLayout()->draw(p, context);
    }
    p->restore();
}

// ---------------------------------------------------------------------------
// Images

QImage componentImage(const QString &path, QSize pixels, bool cover) {
    static QMutex mutex;
    static QCache<QString, QImage> cache(192 * 1024);
    const QFileInfo info(path);
    const QString key = path + ':' + QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' +
                        QString::number(pixels.width()) + 'x' + QString::number(pixels.height()) + (cover ? "c" : "f");
    {
        QMutexLocker lock(&mutex);
        if (const QImage *cached = cache.object(key)) return *cached;
    }
    const QImage image = readSizedImage(path, pixels, cover);
    if (!image.isNull()) {
        QMutexLocker lock(&mutex);
        cache.insert(key, new QImage(image), qMax(1, int(image.sizeInBytes() / 1024)));
    }
    return image;
}

// The bounds of an image's ink: opaque pixels, or non-white ones when it has no transparency.
QRect inkBounds(const QString &path) {
    static QMutex mutex;
    static QHash<QString, QRect> bounds;
    const QString key = path + ':' + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&mutex);
        if (bounds.contains(key)) return bounds.value(key);
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    const qreal scale = size.width() > 800 ? 800.0 / size.width() : 1;
    if (scale < 1) reader.setScaledSize(size * scale);
    const QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
    QRect found;
    if (!image.isNull()) {
        bool transparent = false;
        for (int y = 0; y < image.height() && !transparent; y += qMax(1, image.height() / 24))
            for (int x = 0; x < image.width(); x += qMax(1, image.width() / 24))
                if (qAlpha(image.pixel(x, y)) < 200) {
                    transparent = true;
                    break;
                }
        int left = image.width(), right = -1, top = image.height(), bottom = -1;
        for (int y = 0; y < image.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (transparent ? qAlpha(line[x]) > 24 : qGray(line[x]) < 235)
                    left = qMin(left, x), right = qMax(right, x), top = qMin(top, y), bottom = qMax(bottom, y);
        }
        if (right >= left)
            found = QRect(qFloor(left / scale), qFloor(top / scale), qCeil((right - left + 1) / scale),
                          qCeil((bottom - top + 1) / scale));
    }
    QMutexLocker lock(&mutex);
    bounds.insert(key, found);
    return found;
}

QSize imageSize(const QString &path) {
    static QMutex mutex;
    static QHash<QString, QSize> sizes;
    const QString key = path + ':' + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    QMutexLocker lock(&mutex);
    auto it = sizes.find(key);
    if (it == sizes.end()) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (reader.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
        it = sizes.insert(key, size);
    }
    return *it;
}

// ---------------------------------------------------------------------------
// Layout

namespace {

void overflowed(Ctx &c, const char *why, qreal need, qreal have) {
    if (qEnvironmentVariableIsSet("HYPE_DEBUG_FIT"))
        fprintf(stderr, "  s=%.3f %s: %.1f > %.1f\n", c.s, why, need, have);
    c.overflow = true;
}

enum class Align { Start, Center, End, Stretch };
Align alignOf(const QString &value, Align fallback) {
    const QString v = value.toLower();
    if (v == "start" || v == "left" || v == "top") return Align::Start;
    if (v == "center") return Align::Center;
    if (v == "end" || v == "right" || v == "bottom") return Align::End;
    if (v == "stretch") return Align::Stretch;
    return fallback;
}
Qt::Alignment textAlign(Align a) {
    return a == Align::Center ? Qt::AlignHCenter : a == Align::End ? Qt::AlignRight : Qt::AlignLeft;
}

qreal spacing(const QString &value, qreal fallback, const Ctx &c) {
    static const QHash<QString, qreal> tokens{{"none", 0}, {"xs", 8}, {"sm", 14}, {"md", 24}, {"lg", 40}, {"xl", 64}};
    const QString v = value.trimmed().toLower();
    if (tokens.contains(v)) return c.px(tokens.value(v));
    bool ok = false;
    const qreal number = QString(v).remove("px").toDouble(&ok);
    return c.px(ok ? number : fallback);
}

struct Pad {
    qreal t = 0, r = 0, b = 0, l = 0;
};
Pad padOf(const Node &n, const Ctx &c, qreal fallback = 0) {
    const QStringList parts = n.prop("pad").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    Pad p;
    QVector<qreal> v;
    for (const QString &part : parts) v << spacing(part, fallback, c);
    if (v.isEmpty()) v << c.px(fallback);
    if (v.size() == 1) p = {v[0], v[0], v[0], v[0]};
    else if (v.size() == 2) p = {v[0], v[1], v[0], v[1]};
    else if (v.size() == 3) p = {v[0], v[1], v[2], v[1]};
    else p = {v[0], v[1], v[2], v[3]};
    return p;
}

// A length in slide pixels: "40%" of `whole`, "120px" or a bare number of pixels.
qreal lengthOf(const QString &value, qreal whole, const Ctx &c, bool *ok = nullptr) {
    QString v = value.trimmed().toLower();
    bool converted = false;
    qreal result = 0;
    if (v.endsWith('%')) result = v.chopped(1).toDouble(&converted) / 100 * whole;
    else result = c.px(v.remove("px").toDouble(&converted));
    if (ok) *ok = converted;
    return converted ? result : 0;
}

bool isContainer(const QString &type) {
    return type == "Column" || type == "Row" || type == "Grid" || type == "Flow";
}
bool intrinsic(const Node &n) {
    if (n.prop("width") == "auto") return true;
    if (n.type == "Pill" || n.type == "Icon" || n.type == "QR") return true;
    if (n.type == "Text" && qAbs(n.prop("rotate").toInt()) == 90) return true;
    if (n.type == "Image") return n.has("size") || n.has("height") || n.prop("width").endsWith("px");
    return false;
}
bool isBox(const Node &n) { return n.type == "Row" || n.type == "Column" || n.type == "Grid"; }
// Whether something would use more room than it needs: charts and circular diagrams, spacers,
// whatever the author marks with grow, and any box holding one of those.
bool wantsRoom(const Node &n) {
    if (n.props.contains("grow")) return n.prop("grow").toDouble() > 0 || n.flag("grow");
    if (n.has("height")) return false;
    if (n.type == "Spacer" || n.type == "Chart" || n.type == "Cycle" || n.type == "Venn" || n.type == "Funnel" ||
        n.type == "Tree" || n.type == "Collage")
        return true;
    if (isBox(n) && !n.has("_panel"))
        for (const Node &kid : n.kids)
            if (wantsRoom(kid)) return true;
    return false;
}
qreal growOf(const Node &n) {
    if (n.props.contains("grow")) {
        bool ok = false;
        const qreal weight = n.prop("grow").toDouble(&ok);
        return ok ? qMax(0.0, weight) : (n.flag("grow") ? 1 : 0);
    }
    // A card's body fills the card, without making the card itself reach for room.
    return n.has("_fill") || wantsRoom(n) ? 1 : 0;
}

qreal measure(Node &n, qreal w, const Ink &ink, Ctx &c);
qreal natural(Node &n, const Ink &ink, Ctx &c);
void place(Node &n, const QRectF &r, const Ink &ink, Ctx &c);
void paintNode(Node &n, Ctx &c);

// Ink and inherited text settings for a container's children.
Ink scopeFor(Node &n, const Ink &ink, Ctx &c) {
    Ink in = ink;
    const Fill fill = parseFill(n.prop("fill"), ink, c, n.prop("angle", "90").toDouble());
    n.fillA = fill.a;
    n.fillB = fill.b;
    n.fillAngle = fill.angle;
    if (fill.valid()) in = inkOver(fill.average(), ink, c);
    else if (n.has("image")) in = inkOver(QColor("#202020"), ink, c);
    if (n.has("_text")) in.text = sizeToken(n.prop("_text"), in.text);
    // A column's align is horizontal, so its text follows; a row's or grid's is vertical.
    if (n.type == "Column" && n.has("align") && n.prop("align") != "stretch")
        in.align = textAlign(alignOf(n.prop("align"), Align::Start));
    return in;
}

TextStyle textStyle(const Node &n, const Ink &ink, const Ctx &c) {
    TextStyle st;
    st.size = sizeToken(n.prop("size"), ink.text);
    st.ink = ink.fg;
    if (n.has("tone")) {
        const Fill fill = parseFill(n.prop("tone"), ink, c);
        if (fill.b.isValid()) {
            st.gradient = fill;
            st.ink = fill.average();
        } else if (fill.valid())
            st.ink = fill.a;
    }
    st.accent = n.has("tone") ? st.ink : ink.accent;
    if (const QColor bold = c.tone(c.boldTone, ink); bold.isValid()) {
        st.accent = bold;
        st.boldTone = true;
    }
    st.muted = ink.muted;
    st.align = n.has("align") ? textAlign(alignOf(n.prop("align"), Align::Start)) : ink.align;
    // Type roles: font="number" or weight="title" take what the template sets for that role.
    static const QStringList roles{"heading", "title", "number", "eyebrow", "body"};
    QString role = n.prop("font");
    if (role.startsWith('@')) role = role.mid(1);
    if (!roles.contains(role)) role.clear();
    st.heading = role == "heading" || role == "title" || role == "number";
    QString weight = n.prop("weight");
    if (weight.startsWith('@')) weight = weight.mid(1);
    if (roles.contains(weight)) st.weight = weight == "heading" ? c.headingWeight() : c.roleWeight(weight);
    else {
        st.weight = weightToken(weight, -1);
        if (st.weight < 0 && !role.isEmpty() && role != "heading" && role != "body") st.weight = c.roleWeight(role);
    }
    st.uppercase = n.flag("uppercase");
    st.tracking = n.prop("tracking").toDouble();
    st.italic = n.flag("italic");
    if (n.has("line")) st.lineHeight = n.prop("line").toDouble();
    if (n.has("_table") || n.text.contains('|')) {
        st.tableHeader = c.tone(n.prop("header", "ink"), ink);
        st.zebra = n.flag("zebra", true);
        st.highlightLast = n.prop("highlight") == "last";
    }
    return st;
}

QTextDocument &textDoc(Node &n, const Ink &ink, Ctx &c) {
    // Colors are baked into the document, and placing can change them (over a band).
    const QString colors = ink.fg.name() + ink.accent.name() + ink.muted.name() + QString::number(int(ink.align)) +
                           c.fontOverride + QString::number(c.boldWeight) + c.boldTone;
    if (!n.doc || n.docScale != c.s || n.docInk != colors) {
        n.doc = makeText(n.text, textStyle(n, ink, c), c);
        n.docScale = c.s;
        n.docWidth = -1;
        n.docInk = colors;
    }
    return *n.doc;
}
qreal textHeight(Node &n, qreal w, const Ink &ink, Ctx &c) {
    QTextDocument &doc = textDoc(n, ink, c);
    if (n.docWidth != w) {
        doc.setTextWidth(w);
        n.docWidth = w;
    }
    if (doc.size().width() > w + 1) {
        overflowed(c, qPrintable("text " + n.text.left(30).replace('\n', ' ')), doc.size().width(), w);
    }
    return doc.size().height();
}
qreal textNatural(Node &n, const Ink &ink, Ctx &c) {
    QTextDocument &doc = textDoc(n, ink, c);
    doc.setTextWidth(-1);
    n.docWidth = -1;
    return doc.idealWidth() + 1;
}
// Turned text runs along the slide's vertical: its length is a height and its lines a width.
int rotationOf(const Node &n) {
    const int angle = n.prop("rotate").toInt();
    return n.type == "Text" && (angle == 90 || angle == -90) ? angle : 0;
}
qreal rotatedThickness(Node &n, const Ink &ink, Ctx &c) {
    textNatural(n, ink, c);
    return n.doc->size().height();
}

qreal iconSide(const Node &n, const Ctx &c) {
    static const QHash<QString, qreal> sizes{{"xs", 20}, {"sm", 26}, {"md", 34}, {"lg", 46}, {"xl", 64},
                                             {"2xl", 88}, {"3xl", 120}};
    const QString v = n.prop("size", "md").toLower();
    return c.px(sizes.contains(v) ? sizes.value(v) : sizeToken(v, 34));
}
qreal iconBox(const Node &n, const Ctx &c) {
    const qreal side = iconSide(n, c);
    if (n.has("_marker")) return side * 1.3;
    const QString style = n.prop("style", "plain");
    return style == "plain" ? side : side * 1.8;
}

struct PillMetrics {
    QFont font;
    qreal width, height, padX, icon;
};
PillMetrics pillMetrics(const Node &n, const Ctx &c) {
    PillMetrics m;
    const qreal size = c.type(sizeToken(n.prop("size"), 20));
    m.font = uiFont(c, size, false, 700);
    const QFontMetricsF metrics(m.font);
    m.height = metrics.height() + size * 0.8;
    m.padX = size * 0.9;
    m.icon = n.has("icon") ? size * 1.15 : 0;
    m.width = metrics.horizontalAdvance(n.text) + 2 * m.padX + (m.icon ? m.icon + size * 0.4 : 0);
    const QString variant = n.prop("variant");
    if (variant == "square" || variant == "circle") {
        m.height = metrics.height() + size * 0.9;
        m.width = qMax(m.height, metrics.horizontalAdvance(n.text) + size * 0.9);
    }
    return m;
}

qreal imageAspect(const Node &n, const Ctx &c) {
    if (n.prop("shape") == "circle") return 1;
    const QString aspect = n.prop("aspect");
    if (!aspect.isEmpty()) {
        const QStringList parts = aspect.split(QRegularExpression("[:/x]"));
        if (parts.size() == 2 && parts[1].toDouble() > 0) return parts[0].toDouble() / parts[1].toDouble();
        if (aspect.toDouble() > 0) return aspect.toDouble();
    }
    const QString path = c.asset(n.prop("src"));
    const QSize size = n.flag("trim") ? inkBounds(path).size() : imageSize(path);
    return size.isEmpty() ? 16.0 / 9 : double(size.width()) / size.height();
}

qreal pillExtra(const Node &n, Ctx &c) {
    if (!n.has("pill")) return 0;
    Node pill;
    pill.type = "Pill";
    pill.text = n.prop("pill");
    pill.props["size"] = "sm";
    return pillMetrics(pill, c).height;
}

qreal childWidth(Node &kid, qreal inner, Align align, const Ink &ink, Ctx &c) {
    const QString width = kid.prop("width");
    bool ok = false;
    if (!width.isEmpty() && width != "auto") {
        const qreal w = lengthOf(width, inner, c, &ok);
        if (ok && (width.endsWith('%') || width.endsWith("px") || !kid.has("_panel"))) return qMin(inner, w);
    }
    if (intrinsic(kid)) {
        const qreal w = natural(kid, ink, c);
        return w < 0 ? inner : qMin(inner, w);
    }
    Q_UNUSED(align);
    return inner;
}

qreal gapBetween(const Node &before, qreal gap) {
    return before.has("_eyebrow") ? gap * 0.3 : gap;
}

// Column ----------------------------------------------------------------------

qreal measureColumn(Node &n, qreal w, const Ink &ink, Ctx &c) {
    const Pad pad = padOf(n, c);
    const Ink in = scopeFor(n, ink, c);
    const qreal inner = qMax(0.0, w - pad.l - pad.r);
    const Align align = alignOf(n.prop("align"), Align::Stretch);
    const qreal gap = spacing(n.prop("gap"), 24, c);
    qreal h = 0;
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        h += measure(kid, childWidth(kid, inner, align, in, c), in, c);
        if (i) h += gapBetween(n.kids[i - 1], gap);
    }
    return h + pad.t + pad.b + pillExtra(n, c);
}

void placeColumn(Node &n, const QRectF &r, const Ink &ink, Ctx &c) {
    const Pad pad = padOf(n, c);
    const Ink in = scopeFor(n, ink, c);
    n.ink = in;
    const qreal top = pillExtra(n, c);
    const QRectF inner = r.adjusted(pad.l, pad.t + top, -pad.r, -pad.b);
    const Align align = alignOf(n.prop("align"), Align::Stretch);
    const qreal gap = spacing(n.prop("gap"), 24, c);
    QVector<qreal> widths, heights;
    qreal total = 0, grow = 0;
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        widths << childWidth(kid, inner.width(), align, in, c);
        heights << measure(kid, widths.last(), in, c);
        total += heights.last() + (i ? gapBetween(n.kids[i - 1], gap) : 0);
        grow += growOf(kid);
    }
    qreal extra = inner.height() - total;
    const qreal room = n.has("_panel") ? inner.height() * c.slack : inner.height();
    if (total > room + 1) overflowed(c, "column", total, room);
    extra = qMax(0.0, extra);
    const QString justify = n.prop("justify", "start");
    qreal y = inner.top(), between = 0;
    if (grow > 0 || justify == "stretch") {
        const qreal weights = grow > 0 ? grow : n.kids.size();
        for (int i = 0; i < n.kids.size(); ++i)
            heights[i] += extra * (grow > 0 ? growOf(n.kids[i]) : 1) / qMax(0.001, weights);
    } else if (justify == "center") y += extra / 2;
    else if (justify == "end") y += extra;
    else if (justify == "between" && n.kids.size() > 1) between = extra / (n.kids.size() - 1);
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        if (i) y += gapBetween(n.kids[i - 1], gap) + between;
        qreal x = inner.left();
        const qreal w = widths[i];
        const Align self = alignOf(kid.prop("align-self"), align);
        if (w < inner.width() - 0.5) {
            if (self == Align::Center) x += (inner.width() - w) / 2;
            else if (self == Align::End) x += inner.width() - w;
        }
        place(kid, QRectF(x, y, w, heights[i]), in, c);
        y += heights[i];
    }
}

// Row -------------------------------------------------------------------------

QVector<qreal> rowWidths(Node &n, qreal inner, const Ink &in, Ctx &c, bool *flexible = nullptr) {
    const qreal gap = spacing(n.prop("gap"), 32, c);
    const qreal available = inner - gap * qMax(0, int(n.kids.size()) - 1);
    QVector<qreal> widths(n.kids.size(), -1), shares(n.kids.size(), 0);
    qreal fixed = 0, total = 0;
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        const QString width = kid.prop("width");
        bool ok = false;
        if (width.endsWith('%') || width.endsWith("px")) widths[i] = lengthOf(width, available, c, &ok);
        else if (kid.type == "Divider" && width.isEmpty()) widths[i] = c.px(kid.prop("thickness", "2").toDouble());
        else if (width == "auto" || (width.isEmpty() && intrinsic(kid))) {
            const qreal w = natural(kid, in, c);
            if (w >= 0) widths[i] = w;
            else shares[i] = 1;
        } else {
            const qreal share = width.isEmpty() ? 1 : width.toDouble(&ok);
            shares[i] = ok || width.isEmpty() ? qMax(0.0, share) : 1;
        }
        if (widths[i] >= 0) fixed += widths[i];
        total += shares[i];
    }
    const qreal remaining = available - fixed;
    if (remaining < -1) overflowed(c, "row widths", fixed, available);
    for (int i = 0; i < n.kids.size(); ++i)
        if (widths[i] < 0) widths[i] = total > 0 ? qMax(0.0, remaining) * shares[i] / total : 0;
    if (flexible) *flexible = total > 0;
    return widths;
}

bool cropsInRow(const Node &n) {
    // Pictures crop and dividers stretch to whatever height their row has.
    return (n.type == "Image" && !n.has("size") && !n.has("height") && !n.has("aspect")) || n.type == "Divider";
}

qreal measureRow(Node &n, qreal w, const Ink &ink, Ctx &c) {
    const Pad pad = padOf(n, c);
    const Ink in = scopeFor(n, ink, c);
    const QVector<qreal> widths = rowWidths(n, qMax(0.0, w - pad.l - pad.r), in, c);
    // A picture beside other content crops to their height rather than setting it.
    bool others = false;
    for (const Node &kid : n.kids) others = others || !cropsInRow(kid);
    qreal h = 0;
    for (int i = 0; i < n.kids.size(); ++i) {
        const qreal kh = measure(n.kids[i], widths[i], in, c);
        if (!others || !cropsInRow(n.kids[i])) h = qMax(h, kh);
    }
    return h + pad.t + pad.b;
}

void placeRow(Node &n, const QRectF &r, const Ink &ink, Ctx &c) {
    const Pad pad = padOf(n, c);
    const Ink in = scopeFor(n, ink, c);
    n.ink = in;
    const QRectF inner = r.adjusted(pad.l, pad.t, -pad.r, -pad.b);
    bool flexible = false;
    const QVector<qreal> widths = rowWidths(n, inner.width(), in, c, &flexible);
    const qreal gap = spacing(n.prop("gap"), 32, c);
    qreal used = 0;
    for (qreal w : widths) used += w;
    used += gap * qMax(0, int(n.kids.size()) - 1);
    qreal x = inner.left(), between = 0;
    const QString justify = n.prop("justify", "start");
    if (!flexible && used < inner.width()) {
        const qreal extra = inner.width() - used;
        if (justify == "center") x += extra / 2;
        else if (justify == "end") x += extra;
        else if (justify == "between" && n.kids.size() > 1) between = extra / (n.kids.size() - 1);
    }
    const Align align = alignOf(n.prop("align"), Align::Stretch);
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        const qreal natural = measure(kid, widths[i], in, c);
        if (natural > inner.height() + 1 && !cropsInRow(kid)) overflowed(c, qPrintable("row child " + kid.type), natural, inner.height());
        qreal y = inner.top(), h = inner.height();
        const Align self = kid.type == "Divider" ? Align::Stretch : alignOf(kid.prop("align-self"), align);
        if (self != Align::Stretch && natural < h) {
            if (self == Align::Center) y += (h - natural) / 2;
            else if (self == Align::End) y += h - natural;
            h = natural;
        }
        place(kid, QRectF(x, y, widths[i], h), in, c);
        x += widths[i] + gap + between;
    }
}

// Grid and Flow ---------------------------------------------------------------

int gridColumns(const Node &n) {
    const int count = n.kids.size();
    if (n.has("cols")) return qBound(1, n.prop("cols").toInt(), 12);
    if (count <= 4) return qMax(1, count);
    if (count == 5 || count == 6 || count == 9) return 3;
    return 4;
}

qreal measureGrid(Node &n, qreal w, const Ink &ink, Ctx &c, qreal *rowHeight = nullptr) {
    const Pad pad = padOf(n, c);
    const Ink in = scopeFor(n, ink, c);
    const int cols = gridColumns(n);
    const qreal gap = spacing(n.prop("gap"), 32, c);
    const qreal inner = qMax(0.0, w - pad.l - pad.r);
    const qreal cell = (inner - gap * (cols - 1)) / cols;
    qreal tallest = 0;
    for (Node &kid : n.kids) tallest = qMax(tallest, measure(kid, intrinsic(kid) ? qMin(cell, natural(kid, in, c)) : cell, in, c));
    const int rows = (n.kids.size() + cols - 1) / cols;
    if (rowHeight) *rowHeight = tallest;
    return rows * tallest + gap * qMax(0, rows - 1) + pad.t + pad.b;
}

void placeGrid(Node &n, const QRectF &r, const Ink &ink, Ctx &c) {
    const Pad pad = padOf(n, c);
    const Ink in = scopeFor(n, ink, c);
    n.ink = in;
    const QRectF inner = r.adjusted(pad.l, pad.t, -pad.r, -pad.b);
    const int cols = gridColumns(n);
    const qreal gap = spacing(n.prop("gap"), 32, c);
    const qreal cell = (inner.width() - gap * (cols - 1)) / cols;
    qreal rowHeight = 0;
    measureGrid(n, r.width(), ink, c, &rowHeight);
    const int rows = (n.kids.size() + cols - 1) / cols;
    const qreal needed = rows * rowHeight + gap * qMax(0, rows - 1);
    if (needed > inner.height() + 1) overflowed(c, "grid", needed, inner.height());
    if (rows > 0 && inner.height() > needed && n.flag("grow")) rowHeight = (inner.height() - gap * (rows - 1)) / rows;
    const Align align = alignOf(n.prop("align"), Align::Stretch);
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        const int row = i / cols, col = i % cols;
        const int inRow = qMin(cols, int(n.kids.size()) - row * cols);
        const qreal offset = n.flag("balance", true) && inRow < cols ? (cols - inRow) * (cell + gap) / 2 : 0;
        QRectF slot(inner.left() + offset + col * (cell + gap), inner.top() + row * (rowHeight + gap), cell, rowHeight);
        const qreal w = intrinsic(kid) ? qMin(cell, natural(kid, in, c)) : cell;
        const qreal h = measure(kid, w, in, c);
        QRectF rect(slot.left() + (cell - w) / 2, slot.top(), w, slot.height());
        if (align != Align::Stretch && h < slot.height()) {
            rect.setHeight(h);
            if (align == Align::Center) rect.moveTop(slot.top() + (slot.height() - h) / 2);
            else if (align == Align::End) rect.moveTop(slot.bottom() - h);
        }
        place(kid, rect, in, c);
    }
}

qreal layoutFlow(Node &n, const QRectF &r, const Ink &ink, Ctx &c, bool apply) {
    const Ink in = scopeFor(n, ink, c);
    if (apply) n.ink = in;
    const qreal gap = spacing(n.prop("gap"), 12, c);
    struct Line {
        QVector<int> items;
        qreal width = 0, height = 0;
    };
    QVector<Line> lines(1);
    for (int i = 0; i < n.kids.size(); ++i) {
        Node &kid = n.kids[i];
        qreal w = natural(kid, in, c);
        if (w < 0 || w > r.width()) w = r.width();
        const qreal h = measure(kid, w, in, c);
        Line &line = lines.last();
        if (!line.items.isEmpty() && line.width + gap + w > r.width() + 0.5) lines.append(Line());
        Line &target = lines.last();
        target.width += (target.items.isEmpty() ? 0 : gap) + w;
        target.height = qMax(target.height, h);
        target.items << i;
    }
    qreal y = r.top();
    const Align align = alignOf(n.prop("align"), Align::Start);
    for (const Line &line : lines) {
        qreal x = r.left();
        if (align == Align::Center) x += (r.width() - line.width) / 2;
        else if (align == Align::End) x += r.width() - line.width;
        for (int index : line.items) {
            Node &kid = n.kids[index];
            qreal w = natural(kid, in, c);
            if (w < 0 || w > r.width()) w = r.width();
            if (apply) place(kid, QRectF(x, y, w, line.height), in, c);
            x += w + gap;
        }
        y += line.height + gap;
    }
    return qMax(0.0, y - r.top() - gap);
}

// Leaves ------------------------------------------------------------------------

qreal measureImage(Node &n, qreal w, const Ctx &c) {
    if (n.has("size")) return c.px(sizeToken(n.prop("size"), 160));
    if (n.has("height")) return lengthOf(n.prop("height"), 1080, c);
    return w / imageAspect(n, c);
}

qreal measureLogo(Node &n, const Ctx &c) { return c.px(sizeToken(n.prop("size"), 80)); }

qreal measure(Node &n, qreal w, const Ink &ink, Ctx &c) {
    const FontScope scope(c, n);
    if (n.measuredScale == c.s && qAbs(n.measuredWidth - w) < 0.01) return n.measuredHeight;
    qreal h = 0;
    const QString &t = n.type;
    if (t == "Column") h = measureColumn(n, w, ink, c);
    else if (t == "Row") h = measureRow(n, w, ink, c);
    else if (t == "Grid") h = measureGrid(n, w, ink, c);
    else if (t == "Flow") h = layoutFlow(n, QRectF(0, 0, w, 0), ink, c, false);
    else if (t == "Text") h = rotationOf(n) ? textNatural(n, ink, c) : textHeight(n, w, ink, c);
    else if (t == "Icon") h = iconBox(n, c);
    else if (t == "Pill") h = pillMetrics(n, c).height;
    else if (t == "Image") h = measureImage(n, w, c);
    else if (t == "Logo") h = measureLogo(n, c);
    else if (t == "Divider") h = c.px(n.prop("thickness", "2").toDouble());
    else if (t == "Spacer") h = spacing(n.prop("size"), 0, c);
    else if (isDiagram(t)) {
        n.ink = ink;
        h = measureDiagram(n, w, c);
    }
    if (n.has("height") && t != "Image") {
        bool ok = false;
        const qreal fixed = lengthOf(n.prop("height"), 1080, c, &ok);
        if (ok && !n.prop("height").endsWith('%')) {
            // A box is at least as tall as asked and grows to hold its content, since shrinking the
            // slide shrinks both alike; anything else takes the height it is given.
            if (isContainer(t)) h = qMax(h, fixed);
            else {
                if (h > fixed + 1 && t != "Spacer") overflowed(c, qPrintable("fixed height " + t), h, fixed);
                h = fixed;
            }
        }
    }
    n.measuredScale = c.s;
    n.measuredWidth = w;
    n.measuredHeight = h;
    return h;
}

qreal natural(Node &n, const Ink &ink, Ctx &c) {
    const FontScope scope(c, n);
    const QString &t = n.type;
    if (n.has("width") && n.prop("width") != "auto") {
        bool ok = false;
        const qreal w = lengthOf(n.prop("width"), 1920, c, &ok);
        if (ok && n.prop("width").endsWith("px")) return w;
    }
    if (t == "Pill") return pillMetrics(n, c).width;
    if (t == "QR") return c.px(sizeToken(n.prop("size"), 220));
    if (t == "Icon") return iconBox(n, c);
    if (t == "Text") return rotationOf(n) ? rotatedThickness(n, ink, c) : textNatural(n, ink, c);
    if (t == "Image") {
        if (n.has("size")) return c.px(sizeToken(n.prop("size"), 160));
        if (n.has("height")) return lengthOf(n.prop("height"), 1080, c) * imageAspect(n, c);
        return -1;
    }
    if (t == "Divider") return c.px(n.prop("thickness", "2").toDouble());
    if ((t == "Column" || t == "Row") && n.kids.isEmpty()) return -1; // An empty box fills its place.
    if (t == "Column" || t == "Row") {
        const Pad pad = padOf(n, c);
        const Ink in = scopeFor(n, ink, c);
        const qreal gap = spacing(n.prop("gap"), t == "Row" ? 32 : 24, c);
        qreal w = 0;
        for (int i = 0; i < n.kids.size(); ++i) {
            const qreal kw = natural(n.kids[i], in, c);
            if (kw < 0) return -1;
            w = t == "Row" ? w + kw + (i ? gap : 0) : qMax(w, kw);
        }
        return w + pad.l + pad.r;
    }
    return -1;
}

void place(Node &n, const QRectF &r, const Ink &given, Ctx &c) {
    const FontScope scope(c, n);
    Ink ink = given;
    // Content that sits on a background band, with nothing filled in between, reads against the band.
    if (given.bg == c.color("background") || given.bg == c.rootBg)
        for (const auto &band : c.bands)
            if (band.first.contains(r.center())) ink = inkOver(band.second, given, c);
    n.rect = r;
    n.ink = ink;
    const QString &t = n.type;
    if (t == "Column") placeColumn(n, r, ink, c);
    else if (t == "Row") placeRow(n, r, ink, c);
    else if (t == "Grid") placeGrid(n, r, ink, c);
    else if (t == "Flow") layoutFlow(n, r, ink, c, true);
    else if (t == "Text" && rotationOf(n)) {
        if (textNatural(n, ink, c) > r.height() + 1) overflowed(c, "turned text", n.doc->idealWidth(), r.height());
    } else if (t == "Text") textHeight(n, r.width(), ink, c);
}

// Painting ----------------------------------------------------------------------

qreal radiusOf(const Node &n, const Ctx &c) {
    const QString value = n.prop("radius");
    if (value.isEmpty()) return 0;
    if (value == "theme") return c.px(c.radius());
    if (value == "full") return 1e6;
    return spacing(value, 0, c);
}

QImage grayscale(QImage image) {
    image = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int g = qGray(line[x]);
            line[x] = qRgba(g, g, g, qAlpha(line[x]));
        }
    }
    return image;
}

void drawCover(QPainter *p, const QString &path, const QRectF &rect, const QPainterPath &clip, const QString &position,
               bool gray, bool contain = false, bool trim = false) {
    if (rect.isEmpty()) return;
    QSize pixels = p->deviceTransform().mapRect(rect).size().toSize().expandedTo(QSize(1, 1));
    QRect ink;
    if (trim) {
        // Read enough of the file that the trimmed ink still fills the rectangle sharply.
        ink = inkBounds(path);
        const QSize full = imageSize(path);
        if (!ink.isEmpty() && !full.isEmpty())
            pixels = QSize(qCeil(pixels.width() * double(full.width()) / ink.width()),
                           qCeil(pixels.height() * double(full.height()) / ink.height()));
    }
    QImage image = componentImage(path, pixels, !contain && !trim);
    if (image.isNull()) return;
    if (trim && !ink.isEmpty()) {
        const QSize full = imageSize(path);
        const double sx = double(image.width()) / full.width(), sy = double(image.height()) / full.height();
        image = image.copy(QRect(qFloor(ink.x() * sx), qFloor(ink.y() * sy), qCeil(ink.width() * sx), qCeil(ink.height() * sy)));
    }
    if (gray) image = grayscale(image);
    QSizeF size = image.size();
    size.scale(rect.size(), contain ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
    qreal x = rect.center().x() - size.width() / 2, y = rect.center().y() - size.height() / 2;
    if (position == "top") y = rect.top();
    else if (position == "bottom") y = rect.bottom() - size.height();
    else if (position == "left") x = rect.left();
    else if (position == "right") x = rect.right() - size.width();
    p->save();
    p->setClipPath(clip, Qt::IntersectClip);
    p->drawImage(QRectF(QPointF(x, y), size), image);
    p->restore();
}

void paintBox(Node &n, const QRectF &box, const QPainterPath &path, Ctx &c, const Ink &outer) {
    QPainter *p = c.p;
    const Fill fill{n.fillA, n.fillB, n.fillAngle};
    const QString shadow = n.prop("shadow");
    const bool wantsShadow = shadow == "auto" ? c.lightSlide && fill.valid() && luminance(fill.average()) > 0.75
                                              : n.flag("shadow");
    if (wantsShadow) paintShadow(p, box, radiusOf(n, c), c);
    if (n.has("image")) {
        drawCover(p, c.asset(n.prop("image")), box, path, n.prop("position"), n.flag("grayscale"));
        const qreal overlay = n.props.contains("overlay") ? n.prop("overlay").toDouble() : fill.valid() ? 0 : 0.35;
        if (fill.valid()) {
            p->save();
            p->setOpacity(n.props.contains("overlay") ? overlay : 0.75);
            paintFill(p, path, fill, box);
            p->restore();
        } else if (overlay > 0)
            p->fillPath(path, QColor(0, 0, 0, qRound(qBound(0.0, overlay, 1.0) * 255)));
    } else
        paintFill(p, path, fill, box);
    if (n.has("bar")) {
        const QString side = n.prop("bar");
        const qreal t = c.px(7);
        QRectF strip = side == "left" ? QRectF(box.left(), box.top(), t, box.height())
                       : side == "bottom" ? QRectF(box.left(), box.bottom() - t, box.width(), t)
                                          : QRectF(box.left(), box.top(), box.width(), t);
        p->save();
        p->setClipPath(path, Qt::IntersectClip);
        QPainterPath bar;
        bar.addRect(strip);
        paintFill(p, bar, parseFill(n.prop("bar-tone", "accent"), outer, c), box);
        p->restore();
    }
    QString border = n.prop("border");
    if (border.isEmpty() && n.has("_card") && !c.lightSlide) border = "line";
    if (!border.isEmpty() && border != "false" && border != "none") {
        const QColor color = border == "true" ? outer.line : c.tone(border, outer);
        if (color.isValid()) {
            p->save();
            p->setPen(QPen(color, c.px(1.5)));
            p->setBrush(Qt::NoBrush);
            p->drawPath(path);
            p->restore();
        }
    }
}

void paintPill(const Node &n, const QRectF &r, Ctx &c) {
    QPainter *p = c.p;
    const PillMetrics m = pillMetrics(n, c);
    const QRectF box(r.left(), r.top(), qMin(r.width(), m.width), m.height);
    const QString variant = n.prop("variant", "solid");
    const qreal radius = variant == "square" ? c.px(8) : m.height / 2;
    const QPainterPath path = roundedPath(box, radius, radius, radius, radius);
    const Fill fill = parseFill(n.prop("tone", "accent"), n.ink, c);
    QColor text = n.ink.fg;
    if (variant == "soft") {
        QColor wash = fill.average();
        wash.setAlphaF(0.14);
        p->fillPath(path, wash);
        text = contrast(fill.average(), n.ink.bg) >= 3 ? fill.average() : n.ink.fg;
    } else if (variant == "outline") {
        p->setPen(QPen(fill.average(), c.px(2)));
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);
        text = contrast(fill.average(), n.ink.bg) >= 3 ? fill.average() : n.ink.fg;
    } else {
        paintFill(p, path, fill, box);
        text = inkOver(fill.average(), n.ink, c).fg;
    }
    QRectF label = box.adjusted(m.padX, 0, -m.padX, 0);
    if (variant == "square" || variant == "circle") label = box;
    if (m.icon > 0) {
        paintIcon(p, n.prop("icon"), QRectF(label.left(), box.center().y() - m.icon / 2, m.icon, m.icon), text, 2.2);
        label.setLeft(label.left() + m.icon + m.font.pixelSize() * 0.4);
    }
    drawLabel(p, label, n.text, m.font, text, (variant == "square" || variant == "circle") ? Qt::AlignCenter
                                                                                         : Qt::AlignVCenter | Qt::AlignLeft);
}

void paintIconNode(const Node &n, Ctx &c) {
    QPainter *p = c.p;
    const qreal box = iconBox(n, c), side = iconSide(n, c);
    QRectF square(n.rect.left() + (n.rect.width() - box) / 2, n.rect.top(), box, box);
    if (n.rect.width() > box * 1.5 && n.ink.align == Qt::AlignLeft) square.moveLeft(n.rect.left());
    const QColor tone = n.has("tone") ? c.tone(n.prop("tone"), n.ink) : n.ink.accent;
    if (n.prop("name") == "_dot") {
        const qreal d = side * 0.36;
        p->setPen(Qt::NoPen);
        p->setBrush(tone);
        p->drawEllipse(QPointF(square.center().x(), square.top() + side * 0.62), d / 2, d / 2);
        return;
    }
    const QString style = n.prop("style", "plain");
    const qreal stroke = n.has("stroke") ? n.prop("stroke").toDouble() : 1.75;
    if (style == "plain") {
        paintIcon(p, n.prop("name"), square, tone.isValid() ? tone : n.ink.accent, stroke);
        return;
    }
    QPainterPath shape;
    const qreal r = style == "square" ? box * (n.has("_marker") ? 0.22 : 0.26) : box / 2;
    shape = roundedPath(square, r, r, r, r);
    QColor iconColor = tone;
    if (style == "soft") {
        QColor wash = tone;
        wash.setAlphaF(0.14);
        p->fillPath(shape, wash);
    } else if (style == "outline") {
        p->setPen(QPen(tone, c.px(2)));
        p->setBrush(Qt::NoBrush);
        p->drawPath(shape);
    } else {
        p->fillPath(shape, tone);
        iconColor = inkOver(tone, n.ink, c).fg;
    }
    const qreal inner = n.has("_marker") ? box * 0.7 : side;
    paintIcon(p, n.prop("name"), QRectF(square.center() - QPointF(inner / 2, inner / 2), QSizeF(inner, inner)), iconColor,
              n.has("_marker") ? 3 : stroke);
}

void paintImageNode(const Node &n, Ctx &c) {
    QPainter *p = c.p;
    QRectF r = n.rect;
    const QString shape = n.prop("shape", "rect");
    if (shape == "circle") {
        const qreal side = qMin(r.width(), r.height());
        r = QRectF(r.left() + (r.width() - side) / 2, r.top(), side, side);
    } else if (n.has("size")) {
        const qreal side = c.px(sizeToken(n.prop("size"), 160));
        r = QRectF(r.left() + (r.width() - side) / 2, r.top(), side, qMin(side, r.height()));
    }
    const qreal radius = shape == "circle" ? r.width() / 2 : shape == "rounded" ? c.px(c.radius()) : radiusOf(n, c);
    const QPainterPath path = roundedPath(r, radius, radius, radius, radius);
    if (n.flag("shadow")) paintShadow(p, r, radius, c);
    const QString file = c.asset(n.prop("src"));
    if (!QFileInfo::exists(file)) {
        p->fillPath(path, n.ink.line);
        drawLabel(p, r.adjusted(8, 0, -8, 0), "Missing " + n.prop("src"), uiFont(c, c.px(18)), n.ink.muted);
        return;
    }
    const bool contain = n.prop("fit") == "contain" || n.flag("trim");
    drawCover(p, file, r, path, n.prop("position"), n.flag("grayscale"), contain, n.flag("trim"));
    if (n.flag("play")) paintPlay(p, r, n.ink.accent, c);
    if (n.has("ring") || n.has("border")) {
        const QColor ring = c.tone(n.prop("ring", n.prop("border")), n.ink);
        if (ring.isValid()) {
            const qreal w = c.px(n.has("ring") ? 5 : 1.5);
            p->setPen(QPen(ring, w));
            p->setBrush(Qt::NoBrush);
            p->drawPath(roundedPath(r.adjusted(w / 2, w / 2, -w / 2, -w / 2), radius, radius, radius, radius));
        }
    }
}

QImage logoImage(const QString &path, QSize pixels, const QString &tone, const QColor &color) {
    static QMutex mutex;
    static QCache<QString, QImage> cache(32 * 1024);
    const QString key = path + ':' + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()) + ':' +
                        QString::number(pixels.width()) + 'x' + QString::number(pixels.height()) + tone + color.name();
    {
        QMutexLocker lock(&mutex);
        if (const QImage *cached = cache.object(key)) return *cached;
    }
    QImage image = readSizedImage(path, pixels, false).convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) return image;
    bool transparent = false;
    for (int y = 0; y < image.height() && !transparent; y += qMax(1, image.height() / 32))
        for (int x = 0; x < image.width(); x += qMax(1, image.width() / 32))
            if (qAlpha(image.pixel(x, y)) < 200) {
                transparent = true;
                break;
            }
    // Trim the margins so every logo is measured by its ink, not its file.
    auto ink = [&](QRgb px) {
        return transparent ? qAlpha(px) > 24 : qGray(px) < 235;
    };
    int left = image.width(), right = -1, top = image.height(), bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (ink(line[x])) {
                left = qMin(left, x), right = qMax(right, x), top = qMin(top, y), bottom = qMax(bottom, y);
            }
    }
    if (right >= left && bottom >= top) image = image.copy(left, top, right - left + 1, bottom - top + 1);
    if (tone != "original") {
        for (int y = 0; y < image.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = transparent ? qAlpha(line[x]) : 255 - qGray(line[x]);
                line[x] = qRgba(color.red(), color.green(), color.blue(), qBound(0, alpha * 255 / 230, 255));
            }
        }
    }
    QMutexLocker lock(&mutex);
    cache.insert(key, new QImage(image), qMax(1, int(image.sizeInBytes() / 1024)));
    return image;
}

void paintLogo(const Node &n, Ctx &c) {
    const QString path = c.asset(n.prop("src"));
    const QString tone = n.prop("tone", "original");
    const QColor color = tone == "white" ? QColor(Qt::white) : tone == "black" ? QColor("#111111") : n.ink.fg;
    const QRectF cell = n.rect;
    const QSize pixels = c.p->deviceTransform().mapRect(cell).size().toSize().expandedTo(QSize(1, 1)) * 2;
    const QImage image = logoImage(path, pixels, tone, color);
    if (image.isNull()) return;
    // Equal area, not equal height: wide wordmarks shrink and square marks grow.
    const qreal aspect = double(image.width()) / image.height();
    const qreal height = cell.height() * 0.7;
    const qreal area = height * height * 3.2;
    qreal h = std::sqrt(area / aspect), w = h * aspect;
    const qreal fit = qMin(1.0, qMin(cell.width() / w, cell.height() / h));
    w *= fit, h *= fit;
    c.p->drawImage(QRectF(cell.center().x() - w / 2, cell.center().y() - h / 2, w, h), image);
}

void paintNode(Node &n, Ctx &c) {
    const FontScope scope(c, n);
    QPainter *p = c.p;
    const QString &t = n.type;
    if (isContainer(t)) {
        QRectF box = n.rect;
        const qreal top = pillExtra(n, c);
        box.setTop(box.top() + top / 2);
        const qreal r = radiusOf(n, c);
        const QPainterPath path = roundedPath(box, r, r, r, r);
        Ink outer = n.ink;
        if (n.fillA.isValid()) outer = baseInk(c); // Borders and bars resolve against the slide.
        paintBox(n, box, path, c, outer);
        p->save();
        if (n.flag("clip")) p->setClipPath(path, Qt::IntersectClip);
        if (n.has("_connect") && n.kids.size() > 1 && !n.kids.first().kids.isEmpty() && !n.kids.last().kids.isEmpty()) {
            // A line through the markers joins the items into a sequence.
            const QRectF first = n.kids.first().kids.first().rect, last = n.kids.last().kids.first().rect;
            const qreal side = qMin(first.width(), first.height());
            const qreal x = first.left() + side / 2;
            const QColor color = c.tone(n.prop("_connect"), n.ink);
            p->setPen(QPen(color.isValid() ? color : n.ink.fg, c.px(3)));
            p->drawLine(QPointF(x, first.top() + side / 2), QPointF(x, last.top() + side / 2));
        }
        for (int i = 0; i < n.kids.size(); ++i) {
            paintNode(n.kids[i], c);
            if (n.flag("divided") && i + 1 < n.kids.size()) {
                const qreal y = (n.kids[i].rect.bottom() + n.kids[i + 1].rect.top()) / 2;
                p->setPen(QPen(n.ink.line, c.px(1.5)));
                p->drawLine(QPointF(n.kids[i].rect.left(), y), QPointF(n.rect.right() - padOf(n, c).r, y));
            }
        }
        p->restore();
        if (n.has("pill")) {
            Node pill;
            pill.type = "Pill";
            pill.text = n.prop("pill");
            pill.props = {{"size", "sm"}, {"tone", n.prop("pill-tone", "gradient")}};
            pill.ink = n.ink;
            const PillMetrics m = pillMetrics(pill, c);
            paintPill(pill, QRectF(n.rect.center().x() - m.width / 2, n.rect.top(), m.width, m.height), c);
        }
        return;
    }
    if (t == "Text" && n.doc && rotationOf(n)) {
        p->save();
        p->translate(n.rect.center());
        p->rotate(rotationOf(n));
        const QSizeF size = n.doc->size();
        drawText(*n.doc, QPointF(-size.width() / 2, -size.height() / 2), p);
        p->restore();
    } else if (t == "Text" && n.doc) {
        drawText(*n.doc, n.rect.topLeft(), p);
    } else if (t == "Icon") paintIconNode(n, c);
    else if (t == "Pill") paintPill(n, n.rect, c);
    else if (t == "Image") paintImageNode(n, c);
    else if (t == "Logo") paintLogo(n, c);
    else if (t == "Divider") {
        const QColor color = n.has("tone") ? c.tone(n.prop("tone"), n.ink) : n.ink.line;
        const qreal w = c.px(n.prop("thickness", "2").toDouble());
        if (n.rect.width() >= n.rect.height())
            p->fillRect(QRectF(n.rect.left(), n.rect.center().y() - w / 2, n.rect.width(), w), color);
        else
            p->fillRect(QRectF(n.rect.center().x() - w / 2, n.rect.top(), w, n.rect.height()), color);
    } else if (isDiagram(t))
        paintDiagram(n, c);
}

// The slide's own backdrop, and the ink that text over it should use.
Ink paintBackgrounds(Composition &slide, Ctx &c, bool paint) {
    Ink ink = baseInk(c);
    c.bands.clear();
    if (paint) c.p->fillRect(QRectF(0, 0, 1920, 1080), ink.bg);
    for (Node &bg : slide.backgrounds) {
        QRectF region(0, 0, 1920, 1080);
        const QString band = bg.prop("band");
        if (!band.isEmpty()) {
            const qreal share = lengthOf(bg.prop("size", "50%"), 1, c);
            const qreal fraction = bg.prop("size", "50%").endsWith('%') ? share : share / c.s / 1080;
            if (band == "top") region.setHeight(1080 * fraction);
            else if (band == "bottom") region.setTop(1080 * (1 - fraction));
            else if (band == "left") region.setWidth(1920 * fraction);
            else if (band == "right") region.setLeft(1920 * (1 - fraction));
        }
        const Fill fill = parseFill(bg.prop("fill"), ink, c, bg.prop("angle", "90").toDouble());
        QPainterPath path;
        path.addRect(region);
        const qreal overlay = bg.props.contains("overlay") ? bg.prop("overlay").toDouble() : 0;
        QColor average = fill.valid() ? fill.average() : QColor();
        if (bg.has("image")) {
            const QString file = c.asset(bg.prop("image"));
            if (paint) {
                if (bg.flag("blur")) {
                    QImage small = componentImage(file, QSize(320, 180), true);
                    if (bg.flag("grayscale")) small = grayscale(small);
                    QImage blurred = boxBlur(small, 6);
                    c.p->save();
                    c.p->setClipPath(path);
                    QSizeF size = blurred.size();
                    size.scale(region.size(), Qt::KeepAspectRatioByExpanding);
                    c.p->drawImage(QRectF(region.center() - QPointF(size.width() / 2, size.height() / 2), size), blurred);
                    c.p->restore();
                } else
                    drawCover(c.p, file, region, path, bg.prop("position"), bg.flag("grayscale"));
                if (fill.valid()) {
                    c.p->save();
                    c.p->setOpacity(bg.props.contains("overlay") ? overlay : 0.6);
                    paintFill(c.p, path, fill, region);
                    c.p->restore();
                } else if (overlay > 0)
                    c.p->fillPath(path, QColor(0, 0, 0, qRound(qBound(0.0, overlay, 1.0) * 255)));
            }
            if (!fill.valid()) {
                const QImage tiny = componentImage(file, QSize(32, 18), true).scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                QColor seen = tiny.isNull() ? QColor("#808080") : tiny.pixelColor(0, 0);
                if (bg.flag("grayscale")) seen = QColor(qGray(seen.rgb()), qGray(seen.rgb()), qGray(seen.rgb()));
                average = mix(seen, Qt::black, overlay);
            } else
                average = mix(QColor("#808080"), fill.average(), bg.props.contains("overlay") ? overlay : 0.6);
        } else if (paint)
            paintFill(c.p, path, fill, region);
        if (band.isEmpty() && average.isValid()) ink = inkOver(average, ink, c);
        else if (!band.isEmpty() && average.isValid()) c.bands.append({region, average});
    }
    c.rootBg = ink.bg;
    c.lightSlide = !ink.dark;
    return ink;
}

void layoutOverlays(Composition &slide, const Ink &ink, Ctx &c) {
    for (Node &place : slide.overlays) {
        const QStringList insetParts = place.prop("inset", "44 56").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        const qreal iy = insetParts.isEmpty() ? 44 : insetParts[0].toDouble();
        const qreal ix = insetParts.size() > 1 ? insetParts[1].toDouble() : iy;
        const QString at = place.prop("at", "top-right");
        QRectF area(ix, iy, 1920 - 2 * ix, 1080 - 2 * iy);
        qreal w = place.has("width") ? lengthOf(place.prop("width"), 1920, c) : natural(place, ink, c);
        if (w < 0) w = (at == "top" || at == "bottom" || at == "center") ? area.width() : area.width() * 0.3;
        w = qMin(w, area.width());
        qreal h = place.has("height") ? lengthOf(place.prop("height"), 1080, c) : measure(place, w, ink, c);
        if ((at == "left" || at == "right") && !place.has("height") && h <= 0) h = area.height();
        qreal x = area.left(), y = area.top();
        if (at.endsWith("right")) x = area.right() - w;
        else if (at == "top" || at == "bottom" || at == "center") x = area.center().x() - w / 2;
        if (at.startsWith("bottom")) y = area.bottom() - h;
        else if (at == "left" || at == "right" || at == "center") y = area.center().y() - h / 2;
        place.props.remove("height");
        place.props.remove("width");
        ::place(place, QRectF(x, y, w, h), ink, c);
    }
}

bool isHeader(const Node &n) {
    if (n.has("_eyebrow")) return true;
    return n.type == "Text" && n.text.trimmed().startsWith("# ");
}
// A short plain paragraph right after the title belongs with it.
bool isSubtitle(const Node &n) {
    if (n.type != "Text" || n.has("_eyebrow")) return false;
    const QString t = n.text.trimmed();
    static const QRegularExpression block("^(#|[-*+] |\\d+[.)] |\\||>|```)", QRegularExpression::MultilineOption);
    return t.size() < 320 && !block.match(t).hasMatch();
}

} // namespace

void drawPicture(QPainter *p, const QString &path, const QRectF &rect, const QPainterPath &clip,
                 const QString &position, bool gray, bool contain) {
    drawCover(p, path, rect, clip, position, gray, contain);
}

void paintPlay(QPainter *p, const QRectF &r, const QColor &accent, const Ctx &c) {
    const qreal d = qBound(c.px(56), qMin(r.width(), r.height()) * 0.16, c.px(120));
    const QPointF center = r.center();
    p->save();
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(255, 255, 255, 235));
    p->drawEllipse(center, d / 2, d / 2);
    QPainterPath triangle;
    triangle.moveTo(center + QPointF(-d * 0.12, -d * 0.18));
    triangle.lineTo(center + QPointF(d * 0.2, 0));
    triangle.lineTo(center + QPointF(-d * 0.12, d * 0.18));
    triangle.closeSubpath();
    p->fillPath(triangle, accent);
    p->restore();
}

void paintInitials(QPainter *p, const QRectF &circle, const QString &name, const QColor &fill, const Ctx &c) {
    QString initials;
    const QStringList words = plainText(name).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (!words.isEmpty()) initials += words.first().left(1);
    if (words.size() > 1) initials += words.last().left(1);
    p->save();
    p->setPen(Qt::NoPen);
    p->setBrush(fill);
    p->drawEllipse(circle);
    const QFont font = uiFont(c, circle.height() * 0.38, true);
    drawLabel(p, circle, initials.toUpper(), font, inkOver(fill, baseInk(c), c).fg);
    p->restore();
}

bool layoutComposition(Composition &slide, Ctx &c) {
    c.overflow = false;
    const Ink ink = paintBackgrounds(slide, c, false);
    // Decoration keeps its size however much the content had to shrink.
    Ctx flat = c;
    flat.s = 1;
    layoutOverlays(slide, ink, flat);
    if (slide.bleed) {
        place(slide.flow[0], QRectF(0, 0, 1920, 1080), ink, c);
        return !c.overflow;
    }
    if (slide.flow.isEmpty()) return true;
    const QStringList margin = c.palette.value("margin", "72 96").toString().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const qreal my = margin.value(0, "72").toDouble(), mx = margin.value(1, margin.value(0, "96")).toDouble();
    const QRectF area(mx, my, 1920 - 2 * mx, 1080 - 2 * my);
    const qreal gap = c.px(40);
    int header = 0;
    if (slide.justify == "auto") {
        while (header < slide.flow.size() && isHeader(slide.flow[header])) ++header;
        if (header && header < slide.flow.size() - 1 && isSubtitle(slide.flow[header])) ++header;
    }
    // Headers keep clear of a logo in the corner: centered ones on both sides.
    auto headerBox = [&](const Node &n, qreal top) {
        QRectF box = area;
        for (const Node &overlay : slide.overlays) {
            const QRectF o = overlay.rect.adjusted(-24, -8, 24, 8);
            if (o.bottom() < top || o.top() > top + c.px(160) || o.right() < area.left() || o.left() > area.right()) continue;
            const bool centered = n.prop("align") == "center";
            if (o.center().x() > area.center().x()) {
                const qreal cut = qMax(0.0, area.right() - o.left());
                box.setRight(box.right() - cut);
                if (centered) box.setLeft(box.left() + cut);
            } else {
                const qreal cut = qMax(0.0, o.right() - area.left());
                box.setLeft(box.left() + cut);
                if (centered) box.setRight(box.right() - cut);
            }
        }
        return box;
    };
    QVector<QRectF> columns;
    QVector<qreal> heights;
    qreal total = 0;
    qreal top = area.top();
    for (int i = 0; i < slide.flow.size(); ++i) {
        Node &n = slide.flow[i];
        QRectF column = i < header ? headerBox(n, top) : area;
        const qreal w = childWidth(n, column.width(), Align::Stretch, ink, c);
        const Align self = alignOf(n.prop("align-self"), Align::Start);
        if (w < column.width() - 0.5) {
            if (self == Align::Center) column.setLeft(column.left() + (column.width() - w) / 2);
            else if (self == Align::End) column.setLeft(column.right() - w);
        }
        column.setWidth(w);
        columns << column;
        heights << measure(n, w, ink, c);
        const qreal g = i ? gapBetween(slide.flow[i - 1], gap) : 0;
        total += heights.last() + g;
        top += heights.last() + g;
    }
    if (total > area.height() * c.slack + 1) overflowed(c, "slide", total, area.height() * c.slack);
    const qreal extra = qMax(0.0, area.height() - total);
    const QString justify = slide.justify;
    qreal grow = 0;
    for (int i = header; i < slide.flow.size(); ++i) grow += growOf(slide.flow[i]);
    qreal y = area.top();
    if (grow <= 0) {
        if (justify == "center" || (justify == "auto" && header == slide.flow.size())) y += extra / 2;
        else if (justify == "end") y += extra;
    }
    for (int i = 0; i < slide.flow.size(); ++i) {
        if (i) y += gapBetween(slide.flow[i - 1], gap);
        if (grow <= 0 && justify == "auto" && i == header && header > 0 && header < slide.flow.size()) y += extra / 2;
        if (justify == "between" && i > 0 && slide.flow.size() > 1) y += extra / (slide.flow.size() - 1);
        qreal h = heights[i];
        if (grow > 0 && i >= header) h += extra * growOf(slide.flow[i]) / grow;
        place(slide.flow[i], QRectF(columns[i].left(), y, columns[i].width(), h), ink, c);
        y += h;
    }
    return !c.overflow;
}

void paintComposition(Composition &slide, Ctx &c) {
    c.p->save();
    c.p->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    paintBackgrounds(slide, c, true);
    for (Node &n : slide.flow) paintNode(n, c);
    Ctx flat = c;
    flat.s = 1;
    for (Node &n : slide.overlays) paintNode(n, flat);
    c.p->restore();
}
