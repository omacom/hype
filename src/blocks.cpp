// Components: parsing, templates, validation and desugaring. Layout and painting
// live in layout.cpp; charts and diagrams in diagrams.cpp.
#include "blocks.h"
#include "blocks_p.h"
#include "renderer.h"
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextDocument>
#include <algorithm>

namespace {

// ---------------------------------------------------------------------------
// The vocabulary. Every prop an author may write is listed here, so a typo is an
// error with a suggestion rather than a silently ignored setting.

struct Spec {
    QStringList props;
    QMap<QString, QStringList> choices;
    bool raw = false; // Inner Markdown is data or text, not child components.
    QString summary;
};

const QStringList alignments{"start", "center", "end", "stretch", "left", "right"};
const QStringList justifications{"start", "center", "end", "between", "stretch"};
const QStringList iconStyles{"plain", "circle", "soft", "square", "outline"};
const QStringList positions{"center", "top", "bottom", "left", "right"};
const QStringList common{"width", "height", "grow", "align-self", "font", "bold", "bold-tone"};
const QStringList box{"pad", "fill", "angle", "radius", "border", "shadow", "clip"};

const QHash<QString, Spec> &specs() {
    static const QHash<QString, Spec> table = [] {
        QHash<QString, Spec> s;
        s["Row"] = {QStringList{"gap", "align", "justify"} + box,
                    {{"align", alignments}, {"justify", justifications}},
                    false,
                    "Children side by side"};
        s["Column"] = {QStringList{"gap", "align", "justify"} + box,
                       {{"align", alignments}, {"justify", justifications}},
                       false,
                       "Children stacked top to bottom"};
        s["Grid"] = {QStringList{"cols", "gap", "align", "balance"} + box,
                     {{"align", alignments}},
                     false,
                     "Equal cells in rows"};
        s["Split"] = {{"ratio", "direction", "gap"},
                      {{"direction", {"row", "column"}}},
                      false,
                      "Full-bleed panels across the slide"};
        s["Panel"] = {{"fill", "angle", "image", "overlay", "grayscale", "position", "pad", "gap",
                       "align", "justify", "radius"},
                      {{"align", alignments}, {"justify", justifications}, {"position", positions}},
                      false,
                      "A region of a Split with its own fill or picture"};
        s["Card"] = {{"title", "title-tone",   "icon",     "icon-style", "icon-size", "number",
                      "tone",  "fill",         "angle",    "border",     "shadow",    "radius",
                      "pad",   "gap",          "align",    "justify",    "header",    "accent",
                      "image", "image-height", "text-size"},
                     {{"icon-style", iconStyles},
                      {"align", alignments},
                      {"justify", justifications},
                      {"header", {"none", "tab", "pill"}},
                      {"accent", {"none", "top", "left", "bottom"}}},
                     false,
                     "A surface with an optional image, icon, number, title and body"};
        s["Callout"] = {{"icon", "title", "tone", "fill", "angle", "radius", "pad", "size"},
                        {},
                        false,
                        "A highlighted bar with an icon"};
        s["Quote"] = {{"by", "role", "image", "size", "align", "tone"},
                      {{"align", alignments}},
                      false,
                      "A pull quote with attribution"};
        s["Text"] = {{"size", "tone", "align", "weight", "font", "uppercase", "tracking", "italic",
                      "line", "rotate"},
                     {{"align", alignments}, {"rotate", {"0", "90", "-90"}}},
                     true,
                     "Markdown with type settings"};
        s["Eyebrow"] = {{"tone", "align", "size"},
                        {{"align", alignments}},
                        true,
                        "A small uppercase label above a title"};
        s["Pill"] = {{"tone", "variant", "icon", "size", "align"},
                     {{"variant", {"solid", "soft", "outline", "square"}}, {"align", alignments}},
                     true,
                     "A rounded label"};
        s["Tags"] = {{"tone", "variant", "size", "align", "gap"},
                     {{"variant", {"solid", "soft", "outline", "square"}}, {"align", alignments}},
                     true,
                     "A wrapping row of pills from a list"};
        s["List"] = {{"icon", "icon-style", "tone", "cols", "gap", "divided", "connected", "size"},
                     {{"icon-style", {"plain", "box", "circle", "soft", "number", "dot"}}},
                     true,
                     "A list with icons, numbers or checkboxes"};
        s["Stat"] = {{"value", "label", "caption", "delta", "trend", "good", "icon", "icon-style",
                      "tone", "size", "layout", "align"},
                     {{"trend", {"up", "down", "flat"}},
                      {"good", {"up", "down"}},
                      {"size", {"sm", "md", "lg", "xl"}},
                      {"layout", {"stack", "row"}},
                      {"icon-style", iconStyles},
                      {"align", alignments}},
                     false,
                     "A headline number with label, delta and caption"};
        s["Icon"] = {{"name", "size", "tone", "style", "stroke", "align"},
                     {{"style", iconStyles}, {"align", alignments}},
                     false,
                     "A Lucide icon"};
        s["Image"] = {{"src", "size", "fit", "trim", "play", "shape", "radius", "ring", "grayscale",
                       "aspect", "position", "align", "shadow", "border"},
                      {{"fit", {"cover", "contain"}},
                       {"shape", {"rect", "rounded", "circle"}},
                       {"position", positions},
                       {"align", alignments}},
                      false,
                      "A picture from images/"};
        s["Person"] = {
            {"image", "name", "role", "caption", "size", "layout", "ring", "shape", "align",
             "tone"},
            {{"layout", {"stack", "row"}}, {"shape", {"circle", "rounded"}}, {"align", alignments}},
            false,
            "A portrait with name and role"};
        s["Logos"] = {{"cols", "tone", "size", "gap"},
                      {{"tone", {"original", "mono", "white", "black", "ink"}}},
                      true,
                      "A grid of logos sized to look equal"};
        s["Divider"] = {{"tone", "thickness"}, {}, false, "A hairline"};
        s["Spacer"] = {{"size"}, {}, false, "Empty space that grows"};
        s["Place"] = {{"at", "inset", "pad", "gap", "align", "justify", "fill", "angle", "radius"},
                      {{"at",
                        {"top-left", "top", "top-right", "left", "center", "right", "bottom-left",
                         "bottom", "bottom-right"}},
                       {"align", alignments},
                       {"justify", justifications}},
                      false,
                      "Decoration pinned to a slide edge or corner, outside the flow"};
        s["Background"] = {
            {"image", "fill", "angle", "overlay", "grayscale", "position", "band", "size", "blur"},
            {{"band", {"top", "bottom", "left", "right"}}, {"position", positions}},
            false,
            "A slide backdrop: picture, color, gradient or band"};
        s["Table"] = {{"header", "zebra", "size", "highlight"},
                      {{"highlight", {"none", "first", "last"}}},
                      true,
                      "A styled Markdown table"};
        s["Chart"] = {
            {"type", "colors", "legend", "labels", "min", "max", "goal", "goal-label", "highlight",
             "marks", "unit", "smooth", "size"},
            {{"type",
              {"bar", "column", "line", "area", "donut", "pie", "stacked-bar", "stacked-column"}},
             {"legend", {"auto", "none", "top", "inline"}},
             {"labels", {"auto", "none", "all", "end"}}},
            true,
            "A bar, column, line, area, donut or pie chart from a Markdown table"};
        s["Progress"] = {{"value", "max", "label", "tone", "show", "size", "thickness"},
                         {{"show", {"value", "percent", "none"}}},
                         true,
                         "A progress bar, or one per table row"};
        s["Gantt"] = {
            {"start", "end", "unit", "today", "colors", "tone", "size", "labels", "groups"},
            {{"unit", {"auto", "day", "week", "month", "quarter"}},
             {"labels", {"left", "inside"}},
             {"groups", {"top", "side"}}},
            true,
            "Bars on a time axis from a Markdown table"};
        s["Timeline"] = {
            {"direction", "tone", "style", "size", "colors"},
            {{"direction", {"horizontal", "vertical"}}, {"style", {"dot", "number", "icon"}}},
            true,
            "Milestones along a line from a list"};
        s["Steps"] = {
            {"style", "direction", "tone", "numbered", "size", "colors", "hub", "hub-tone"},
            {{"style", {"chevron", "arrow", "card", "circle"}},
             {"direction", {"horizontal", "vertical"}}},
            true,
            "A process from a list"};
        s["Cycle"] = {
            {"center", "center-image", "phases", "tone", "numbered", "size", "colors", "style"},
            {{"style", {"ring", "wheel"}}},
            true,
            "A loop of stages from a list, as a ring or a wheel"};
        s["Funnel"] = {{"colors", "tone", "size", "labels", "axis"},
                       {{"labels", {"inside", "side"}}},
                       true,
                       "Narrowing stages from a list"};
        s["Venn"] = {{"center", "center-image", "colors", "size"},
                     {},
                     true,
                     "Two, three or four overlapping circles from a list"};
        s["Tree"] = {{"tone", "size", "avatars", "colors"},
                     {{"avatars", {"auto", "none"}}},
                     true,
                     "An org chart or hierarchy from a nested list"};
        s["QR"] = {{"value", "size", "tone", "label", "align"},
                   {{"align", alignments}},
                   false,
                   "A QR code for a link"};
        s["Collage"] = {{"rows", "gap", "radius", "grayscale"},
                        {},
                        true,
                        "Photos from a list of images, cropped into tidy rows"};
        s["Video"] = {
            {"src", "poster", "aspect", "radius", "autoplay", "loop", "muted", "shadow", "play"},
            {},
            false,
            "A video from videos/, played in place while presenting"};
        return s;
    }();
    return table;
}
const QHash<QString, QString> &aliases() {
    static const QHash<QString, QString> table{{"Stack", "Column"},
                                               {"Columns", "Row"},
                                               {"Badge", "Pill"},
                                               {"Tag", "Pill"},
                                               {"Checklist", "List"}};
    return table;
}
QString canonical(const QString &type) { return aliases().value(type, type); }
bool allowsProp(const Spec &spec, const QString &key) {
    return spec.props.contains(key) || common.contains(key);
}

// Optimal string alignment: a swapped pair of letters ("Crad") counts as one edit.
int editDistance(const QString &a, const QString &b) {
    const QString x = a.toLower(), y = b.toLower();
    QVector<QVector<int>> d(x.size() + 1, QVector<int>(y.size() + 1));
    for (int i = 0; i <= x.size(); ++i)
        d[i][0] = i;
    for (int j = 0; j <= y.size(); ++j)
        d[0][j] = j;
    for (int i = 1; i <= x.size(); ++i)
        for (int j = 1; j <= y.size(); ++j) {
            const int cost = x[i - 1] == y[j - 1] ? 0 : 1;
            d[i][j] = qMin(qMin(d[i - 1][j] + 1, d[i][j - 1] + 1), d[i - 1][j - 1] + cost);
            if (i > 1 && j > 1 && x[i - 1] == y[j - 2] && x[i - 2] == y[j - 1])
                d[i][j] = qMin(d[i][j], d[i - 2][j - 2] + 1);
        }
    return d[x.size()][y.size()];
}
QString closest(const QString &word, const QStringList &options) {
    QString best;
    int bestDistance = qMax(2, int(word.size()) / 2) + 1;
    for (const QString &option : options) {
        const int d = editDistance(word, option);
        if (d < bestDistance ||
            (option.startsWith(word, Qt::CaseInsensitive) && d <= word.size())) {
            best = option;
            bestDistance = d;
        }
    }
    return best;
}
QString suggestion(const QString &word, const QStringList &options, const QString &format = "%1") {
    const QString near = closest(word, options);
    return near.isEmpty() ? QString() : "; did you mean " + format.arg(near) + "?";
}

// ---------------------------------------------------------------------------
// Parsing. Tags start a line; Markdown between them becomes Text nodes.

QString blankComments(const QString &source) {
    const QString visible = outsideCode(source, false);
    QString out = source;
    auto matches = QRegularExpression("<!--[\\s\\S]*?-->").globalMatch(visible);
    while (matches.hasNext()) {
        const auto m = matches.next();
        for (int i = m.capturedStart(); i < m.capturedEnd(); ++i)
            if (out[i] != '\n')
                out[i] = ' ';
    }
    return out;
}

QString dedent(const QStringList &lines) {
    int first = 0, last = lines.size() - 1;
    while (first <= last && lines[first].trimmed().isEmpty())
        ++first;
    while (last >= first && lines[last].trimmed().isEmpty())
        --last;
    int indent = 1 << 20;
    for (int i = first; i <= last; ++i) {
        if (lines[i].trimmed().isEmpty())
            continue;
        int n = 0;
        while (n < lines[i].size() && (lines[i][n] == ' ' || lines[i][n] == '\t'))
            ++n;
        indent = qMin(indent, n);
    }
    QStringList out;
    for (int i = first; i <= last; ++i)
        out << lines[i].mid(qMin(indent, int(lines[i].size())));
    return out.join('\n');
}

const QRegularExpression &openTag() {
    static const QRegularExpression re("^<([A-Z][A-Za-z0-9]*)(?=[\\s/>]|$)");
    return re;
}
const QRegularExpression &fenceMarker() {
    static const QRegularExpression re("^ {0,3}(`{3,}|~{3,})(.*)$");
    return re;
}
// Tracks fenced code across lines; returns true for lines that belong to a fence.
struct FenceState {
    QChar mark;
    int length = 0;
    bool step(const QString &line) {
        const auto m = fenceMarker().match(line);
        if (!m.hasMatch())
            return length > 0;
        const QString run = m.captured(1);
        if (!length) {
            mark = run[0];
            length = run.size();
        } else if (run[0] == mark && run.size() >= length && m.captured(2).trimmed().isEmpty())
            length = 0;
        return true;
    }
};

struct Header {
    QString name;
    QMap<QString, QString> props;
    bool selfClosing = false, ok = false;
    QString rest, error;
    int endLine = 0;
};

Header readHeader(const QStringList &lines, int start) {
    Header h;
    h.endLine = start;
    QString text;
    QChar quote;
    int braces = 0, end = -1;
    for (int j = start; j < lines.size() && j < start + 40 && end < 0; ++j) {
        const QString line = j == start ? lines[j].trimmed() : lines[j];
        if (j > start)
            text += '\n';
        const int from = text.size();
        text += line;
        for (int k = from; k < text.size(); ++k) {
            const QChar ch = text[k];
            if (!quote.isNull()) {
                if (ch == '\\')
                    ++k;
                else if (ch == quote)
                    quote = QChar();
            } else if (braces) {
                if (ch == '{')
                    ++braces;
                else if (ch == '}')
                    --braces;
            } else if (ch == '"' || ch == '\'')
                quote = ch;
            else if (ch == '{')
                braces = 1;
            else if (ch == '>') {
                end = k;
                h.endLine = j;
                h.rest = text.mid(k + 1);
                break;
            }
        }
    }
    const auto name = openTag().match(text);
    h.name = name.captured(1);
    if (end < 0) {
        h.error = "The <" + h.name + "> tag is missing its closing >";
        return h;
    }
    QString attrs = text.mid(name.capturedEnd(), end - name.capturedEnd()).trimmed();
    if (attrs.endsWith('/')) {
        h.selfClosing = true;
        attrs.chop(1);
    }
    static const QRegularExpression attr(
        R"re(([A-Za-z_][\w:-]*)(?:\s*=\s*(?:"((?:[^"\\]|\\.)*)"|'([^']*)'|\{([^}]*)\}|([^\s"'=<>`]+)))?)re");
    int consumed = 0;
    auto it = attr.globalMatch(attrs);
    while (it.hasNext()) {
        const auto m = it.next();
        if (!attrs.mid(consumed, m.capturedStart() - consumed).trimmed().isEmpty())
            break;
        consumed = m.capturedEnd();
        QString value;
        if (m.capturedStart(2) >= 0)
            value = m.captured(2).replace("\\\"", "\"").replace("\\\\", "\\");
        else if (m.capturedStart(3) >= 0)
            value = m.captured(3);
        else if (m.capturedStart(4) >= 0) {
            value = m.captured(4).trimmed();
            if (value.size() >= 2 && (value.startsWith('"') || value.startsWith('\'')) &&
                value.back() == value[0])
                value = value.mid(1, value.size() - 2);
        } else if (m.capturedStart(5) >= 0)
            value = m.captured(5);
        else
            value = "true";
        h.props.insert(m.captured(1), value);
    }
    if (!attrs.mid(consumed).trimmed().isEmpty()) {
        h.error = "Can't read the props of <" + h.name + "> near “" +
                  attrs.mid(consumed).trimmed().left(24) + "”; write them as name=\"value\"";
        return h;
    }
    h.ok = true;
    return h;
}

bool opensSame(const QString &trimmed, const QString &name) {
    const auto m = openTag().match(trimmed);
    return m.hasMatch() && m.captured(1) == name && !trimmed.endsWith("/>") &&
           !trimmed.contains("</" + name + ">");
}

void parseLines(const QStringList &lines, int offset, QVector<Node> &out, QStringList &errors,
                int depth);

void finishNode(Node &node, const QStringList &inner, int innerOffset, QStringList &errors,
                int depth) {
    const auto spec = specs().constFind(canonical(node.type));
    node.text = dedent(inner);
    int leading = 0;
    while (leading < inner.size() && inner[leading].trimmed().isEmpty())
        ++leading;
    node.textLine = innerOffset + leading + 1;
    // Template components are parsed once their definition is filled in, so their content
    // is checked once, against the lines it was written on.
    if (spec == specs().cend() || spec->raw)
        return;
    if (depth < 24)
        parseLines(inner, innerOffset, node.kids, errors, depth + 1);
}

void parseLines(const QStringList &input, int offset, QVector<Node> &out, QStringList &errors,
                int depth) {
    // Tags that follow one another on a line are taken one at a time: the rest of the line
    // becomes the next line, keeping the number of the line it came from.
    QStringList lines = input;
    QVector<int> number(lines.size());
    for (int k = 0; k < number.size(); ++k)
        number[k] = k;
    auto continueWith = [&](int after, const QString &rest) {
        if (rest.trimmed().isEmpty())
            return;
        lines.insert(after + 1, rest.trimmed());
        number.insert(after + 1, number[after]);
    };
    QStringList chunk;
    int chunkLine = 0;
    auto addText = [&](const QStringList &lines, int first, const QMap<QString, QString> &props) {
        const QString markdown = dedent(lines);
        if (markdown.trimmed().isEmpty())
            return;
        Node text;
        text.type = "Text";
        text.text = markdown;
        text.props = props;
        text.line = offset + number.value(first, first) + 1;
        out.append(text);
    };
    auto flush = [&] {
        // `# Title {align=center tone=accent}` sets props on that heading alone, as in Pandoc.
        static const QRegularExpression attributed(
            "^(\\s{0,3}#{1,6}\\s.*?)\\s*\\{([^{}]*=[^{}]*)\\}\\s*$");
        static const QRegularExpression pair(
            "([A-Za-z][\\w-]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"']+))");
        QStringList part;
        int partLine = chunkLine;
        FenceState fence;
        for (int k = 0; k < chunk.size(); ++k) {
            const bool code = fence.step(chunk[k]);
            const auto m = code ? QRegularExpressionMatch() : attributed.match(chunk[k]);
            if (!m.hasMatch()) {
                if (part.isEmpty())
                    partLine = chunkLine + k;
                part << chunk[k];
                continue;
            }
            addText(part, partLine, {});
            part.clear();
            QMap<QString, QString> props;
            for (auto it = pair.globalMatch(m.captured(2)); it.hasNext();) {
                const auto a = it.next();
                props.insert(a.captured(1), a.capturedStart(2) >= 0   ? a.captured(2)
                                            : a.capturedStart(3) >= 0 ? a.captured(3)
                                                                      : a.captured(4));
            }
            addText({m.captured(1)}, chunkLine + k, props);
        }
        addText(part, partLine, {});
        chunk.clear();
    };
    FenceState fence;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        const QString trimmed = line.trimmed();
        if (fence.step(line)) {
            if (chunk.isEmpty())
                chunkLine = i;
            chunk << line;
            continue;
        }
        if (openTag().match(trimmed).hasMatch()) {
            flush();
            const Header h = readHeader(lines, i);
            if (!h.ok) {
                errors << h.error + " (slide line " + QString::number(offset + number[i] + 1) + ")";
                i = h.endLine;
                continue;
            }
            Node node;
            node.type = h.name;
            node.props = h.props;
            node.line = offset + number[i] + 1;
            if (h.selfClosing) {
                i = h.endLine;
                out.append(node);
                continueWith(i, h.rest);
                continue;
            }
            const QString close = "</" + h.name + ">";
            const int inlineClose = h.rest.indexOf(close);
            if (inlineClose >= 0) {
                finishNode(node, {h.rest.left(inlineClose)}, offset + number[h.endLine], errors,
                           depth);
                i = h.endLine;
                out.append(node);
                continueWith(i, h.rest.mid(inlineClose + close.size()));
                continue;
            }
            QStringList inner;
            int innerStart = number.value(h.endLine + 1, number[h.endLine] + 1);
            if (!h.rest.trimmed().isEmpty()) {
                inner << h.rest;
                innerStart = number[h.endLine];
            }
            int nested = 0, j = h.endLine + 1;
            bool closed = false;
            FenceState innerFence;
            for (; j < lines.size(); ++j) {
                const QString t = lines[j].trimmed();
                if (innerFence.step(lines[j])) {
                    inner << lines[j];
                    continue;
                }
                if (opensSame(t, h.name))
                    ++nested;
                if (t.contains(close)) {
                    if (!nested) {
                        const int at = lines[j].indexOf(close);
                        if (!lines[j].left(at).trimmed().isEmpty())
                            inner << lines[j].left(at);
                        continueWith(j, lines[j].mid(at + close.size()));
                        closed = true;
                        break;
                    }
                    --nested;
                }
                inner << lines[j];
            }
            if (!closed)
                errors << "<" + h.name + "> on slide line " + QString::number(node.line) +
                              " is never closed; add " + close + " on its own line";
            finishNode(node, inner, offset + innerStart, errors, depth);
            out.append(node);
            i = closed ? j : lines.size();
            continue;
        }
        static const QRegularExpression stray("^</([A-Z][A-Za-z0-9]*)>");
        if (const auto m = stray.match(trimmed); m.hasMatch()) {
            errors << "Unexpected </" + m.captured(1) + "> on slide line " +
                          QString::number(offset + number[i] + 1) + " with no matching <" +
                          m.captured(1) + ">";
            continue;
        }
        if (chunk.isEmpty())
            chunkLine = i;
        chunk << line;
    }
    flush();
}

// The first and last line of each <Define> block, outside fenced code. Comments should
// already be blank.
QVector<QPair<int, int>> definitionBlocks(const QStringList &lines) {
    QVector<QPair<int, int>> blocks;
    FenceState fence;
    for (int i = 0; i < lines.size(); ++i) {
        if (fence.step(lines[i]) || openTag().match(lines[i].trimmed()).captured(1) != "Define")
            continue;
        int last = i + 1;
        while (last < lines.size() - 1 && lines[last].trimmed() != "</Define>")
            ++last;
        blocks.append({i, qMin(last, int(lines.size()) - 1)});
        i = last;
    }
    return blocks;
}

// A definition belongs to the whole presentation, so the slide it is written on shows none of it.
QString blankDefinitions(const QString &source) {
    if (!source.contains("<Define"))
        return source;
    QStringList lines = source.split('\n');
    // Comments keep their lines, so a <Define> written inside one is not a definition.
    for (const auto &[first, last] : definitionBlocks(blankComments(source).split('\n')))
        for (int i = first; i <= last; ++i)
            lines[i].fill(' ');
    return lines.join('\n');
}

QVector<Node> parse(const QString &source, QStringList &errors) {
    QVector<Node> nodes;
    parseLines(blankDefinitions(blankComments(source)).split('\n'), 0, nodes, errors, 0);
    return nodes;
}

// ---------------------------------------------------------------------------
// Templates: <Define name="X" default="…">body</Define> blocks become components.

struct Definition {
    QMap<QString, QString> defaults;
    QString body;
    QSet<QString> placeholders;
    QString frame; // The frame for slides built on this component; "none" for no frame.
    bool hasFrame = false;
};
struct Definitions {
    QHash<QString, Definition> byName;
    QStringList errors;
    bool pages = false; // Some definition shows page numbers.
};

const QRegularExpression &placeholder() {
    static const QRegularExpression re("\\{([a-z][a-z0-9_-]*)\\}");
    return re;
}

// `slide` numbers errors by the lines of a slide rather than of a template file.
Definitions readDefinitions(const QString &text, bool slide = false) {
    Definitions result;
    if (text.trimmed().isEmpty())
        return result;
    const QString place = slide ? "Slide line " : "Template line ";
    QStringList errors;
    QVector<Node> nodes;
    // A Define's body is template source, expanded per use; parse only its outline here.
    QStringList lines = blankComments(text).split('\n');
    FenceState fence;
    for (int i = 0; i < lines.size(); ++i) {
        if (fence.step(lines[i]))
            continue;
        if (openTag().match(lines[i].trimmed()).captured(1) != "Define")
            continue;
        const Header h = readHeader(lines, i);
        if (!h.ok) {
            result.errors << place + QString::number(i + 1) + ": " + h.error;
            continue;
        }
        QStringList body;
        if (!h.rest.trimmed().isEmpty())
            body << h.rest;
        int j = h.endLine + 1;
        bool closed = false;
        for (; j < lines.size(); ++j) {
            if (lines[j].trimmed() == "</Define>") {
                closed = true;
                break;
            }
            body << lines[j];
        }
        const QString name = h.props.value("name");
        if (!closed)
            result.errors << place + QString::number(i + 1) + ": <Define> is never closed";
        else if (!QRegularExpression("^[A-Z][A-Za-z0-9]*$").match(name).hasMatch())
            result.errors << place + QString::number(i + 1) +
                                 ": <Define> needs name=\"PascalCase\"";
        else if (specs().contains(canonical(name)))
            result.errors << place + QString::number(i + 1) + ": <" + name +
                                 "> is a built-in component; choose another name";
        else {
            Definition d;
            d.defaults = h.props;
            d.defaults.remove("name");
            d.hasFrame = d.defaults.contains("frame");
            d.frame = d.defaults.take("frame");
            d.body = dedent(body);
            auto m = placeholder().globalMatch(d.body);
            while (m.hasNext())
                d.placeholders.insert(m.next().captured(1));
            result.byName.insert(name, d);
        }
        i = j;
    }
    return result;
}

// Templates are registered once and named by a digest of their text, so a slide's palette
// (and every thumbnail URL built from it) carries a short name rather than the whole file.
QMutex registryMutex;
QHash<QString, Definitions> registry;
QStringList registryOrder;

// A copy: the hash inside is shared, not copied, and stays valid if the entry is replaced.
Definitions definitions(const QString &id) {
    QMutexLocker lock(&registryMutex);
    return registry.value(id);
}

// Text with the slide line each of its lines came from.
struct Lines {
    QStringList text;
    QVector<int> origin;
};
Lines trimmedLines(const Lines &in) {
    int first = 0, last = in.text.size() - 1;
    while (first <= last && in.text[first].trimmed().isEmpty())
        ++first;
    while (last >= first && in.text[last].trimmed().isEmpty())
        --last;
    Lines out;
    const QStringList kept = dedent(in.text.mid(first, last - first + 1)).split('\n');
    for (int i = 0; i <= last - first; ++i) {
        out.text << kept.value(i);
        out.origin << in.origin[first + i];
    }
    return out;
}

// A filled-in definition, remembering for each line whether the author wrote it and where.
struct Built {
    QString text;
    QVector<int> origin;
    QVector<bool> authored;
    bool started = false;
    Built() { newLine(); }
    void newLine() {
        origin << 0;
        authored << false;
        started = false;
    }
    // A line belongs to whoever wrote its first visible character.
    void add(const QString &piece, const std::function<int(int)> &from, bool own) {
        const QStringList parts = piece.split('\n');
        for (int k = 0; k < parts.size(); ++k) {
            if (k) {
                text += '\n';
                newLine();
            }
            if (!started && !parts[k].trimmed().isEmpty()) {
                origin.last() = from(k);
                authored.last() = own;
                started = true;
            }
            text += parts[k];
        }
    }
};

void expand(QVector<Node> &nodes, const Definitions &defs, const QVariantMap &palette,
            QStringList &errors, int depth) {
    for (int i = 0; i < nodes.size(); ++i) {
        const auto def = defs.byName.constFind(nodes[i].type);
        if (def == defs.byName.cend()) {
            expand(nodes[i].kids, defs, palette, errors, depth);
            continue;
        }
        const Node use = nodes[i];
        if (depth > 8) {
            errors << "<" + use.type + "> on slide line " + QString::number(use.line) +
                          " nests templates too deeply";
            nodes.remove(i--);
            continue;
        }
        QStringList known =
            def->defaults.keys() + QStringList(def->placeholders.begin(), def->placeholders.end());
        for (auto it = use.props.cbegin(); it != use.props.cend(); ++it)
            if (!known.contains(it.key()) && !common.contains(it.key()))
                errors << "<" + use.type + "> on slide line " + QString::number(use.line) +
                              " has no prop “" + it.key() + "”" + suggestion(it.key(), known);
        // <Slot name="x">…</Slot> inside a use fills {x}; the rest is {children}.
        Lines children;
        QMap<QString, Lines> filled;
        {
            static const QRegularExpression open(
                "^\\s*<Slot\\s+name\\s*=\\s*\"([a-z][a-z0-9_-]*)\"\\s*>(.*)$");
            const QStringList lines = use.text.split('\n');
            for (int k = 0; k < lines.size(); ++k) {
                const int origin = use.textLine > 0 ? use.textLine + k : use.line;
                const auto m = open.match(lines[k]);
                if (!m.hasMatch()) {
                    children.text << lines[k];
                    children.origin << origin;
                    continue;
                }
                Lines slot;
                QString rest = m.captured(2);
                int j = k;
                bool closed = rest.contains("</Slot>");
                if (closed)
                    rest = rest.left(rest.indexOf("</Slot>"));
                if (!rest.trimmed().isEmpty()) {
                    slot.text << rest;
                    slot.origin << origin;
                }
                for (++j; !closed && j < lines.size(); ++j) {
                    const int at = lines[j].indexOf("</Slot>");
                    const int line = use.textLine > 0 ? use.textLine + j : use.line;
                    if (at >= 0) {
                        if (!lines[j].left(at).trimmed().isEmpty()) {
                            slot.text << lines[j].left(at);
                            slot.origin << line;
                        }
                        closed = true;
                        break;
                    }
                    slot.text << lines[j];
                    slot.origin << line;
                }
                if (!closed)
                    errors << "<Slot> on slide line " + QString::number(origin) +
                                  " is never closed; add </Slot>";
                filled.insert(m.captured(1), trimmedLines(slot));
                k = j;
            }
        }
        for (const QString &name : filled.keys())
            if (!def->placeholders.contains(name))
                errors << "<" + use.type + "> on slide line " + QString::number(use.line) +
                              " has no slot “" + name + "”" +
                              suggestion(name, QStringList(def->placeholders.begin(),
                                                           def->placeholders.end()));
        Built body;
        const auto fromUse = [&](int) { return use.line; };
        auto addLines = [&](const Lines &lines) {
            body.add(
                lines.text.join('\n'), [&](int k) { return lines.origin.value(k, use.line); },
                use.textAuthored);
        };
        int last = 0;
        auto m = placeholder().globalMatch(def->body);
        while (m.hasNext()) {
            const auto match = m.next();
            body.add(def->body.mid(last, match.capturedStart() - last), fromUse, false);
            const QString key = match.captured(1);
            if (key == "children")
                addLines(children);
            else if (filled.contains(key))
                addLines(filled.value(key));
            else if (use.props.contains(key))
                body.add(use.props.value(key), fromUse, false);
            else if (def->defaults.contains(key))
                body.add(def->defaults.value(key), fromUse, false);
            else if (key == "page" || key == "pages" || key == "deck-title")
                body.add(palette.value(key == "deck-title" ? "title" : key).toString(), fromUse,
                         false);
            last = match.capturedEnd();
        }
        body.add(def->body.mid(last), fromUse, false);
        // Lines the author wrote keep their own numbers and checks; the template's own lines
        // point at the use and allow the empty props a definition leaves unset.
        auto at = [&](int line, bool *own) {
            const int index = line - 1;
            *own = index >= 0 && index < body.authored.size() && body.authored[index];
            return *own ? body.origin[index] : use.line;
        };
        QVector<Node> made;
        QStringList inner;
        parseLines(blankComments(body.text).split('\n'), 0, made, inner, 0);
        static const QRegularExpression slideLine("slide line (\\d+)");
        for (QString error : inner) {
            bool own = true;
            QString mapped;
            int lastEnd = 0;
            for (auto it = slideLine.globalMatch(error); it.hasNext();) {
                const auto found = it.next();
                bool mine = false;
                const int line = at(found.captured(1).toInt(), &mine);
                own = own && mine;
                mapped += error.mid(lastEnd, found.capturedStart() - lastEnd) + "slide line " +
                          QString::number(line);
                lastEnd = found.capturedEnd();
            }
            mapped += error.mid(lastEnd);
            errors << (own ? mapped
                           : "In <" + use.type + "> on slide line " + QString::number(use.line) +
                                 ": " + mapped);
        }
        std::function<void(QVector<Node> &)> place = [&](QVector<Node> &list) {
            for (Node &n : list) {
                bool own = false;
                n.line = at(n.line, &own);
                n.templated = !own;
                if (n.textLine > 0) {
                    bool authored = false;
                    const int line = at(n.textLine, &authored);
                    n.textAuthored = authored;
                    n.textLine = authored ? line : 0;
                }
                place(n.kids);
            }
        };
        place(made);
        expand(made, defs, palette, errors, depth + 1);
        if (made.size() == 1)
            for (const QString &key : common)
                if (use.props.contains(key))
                    made[0].props[key] = use.props[key];
        nodes.remove(i);
        for (int k = 0; k < made.size(); ++k)
            nodes.insert(i + k, made[k]);
        i += made.size() - 1;
    }
}

// ---------------------------------------------------------------------------
// Validation

bool isColorProp(const QString &type, const QString &key) {
    if (key == "tone" || key == "fill" || key == "ring")
        return !(type == "Logos" && key == "tone");
    if (key == "header")
        return type == "Table";
    if (key == "border" || key == "bold-tone")
        return true;
    return false;
}

void validateTone(const Node &n, const QString &key, const QString &value, const Ctx &c,
                  QStringList &errors) {
    if (value.isEmpty() ||
        (key == "border" && QStringList{"true", "false", "none"}.contains(value)))
        return;
    // Fills, and the color of text, may be gradients: two colors, or the theme's "gradient".
    const bool gradient = key == "fill" || (key == "tone" && canonical(n.type) == "Text");
    if (key == "fill" && (value == "none" || value == "transparent"))
        return;
    if (gradient ? parseFill(value, baseInk(c), c).valid() : c.tone(value, baseInk(c)).isValid())
        return;
    errors << "<" + n.type + "> on slide line " + QString::number(n.line) + ": unknown color “" +
                  value + "”; use " + toneNames(c).join(", ") + ", " +
                  (gradient ? "two colors for a gradient, " : "") + "or #rrggbb";
}

void validateIcon(const Node &n, const QString &name, QStringList &errors) {
    if (name.isEmpty() || iconExists(name))
        return;
    errors << "<" + n.type + "> on slide line " + QString::number(n.line) + ": no icon named “" +
                  name + "”; try " + iconSuggestions(name).join(", ") +
                  " (hype icons <word> searches)";
}

void validateImage(const Node &n, const QString &file, const Ctx &c, QStringList &errors) {
    if (file.isEmpty())
        return;
    const QString path = c.asset(file);
    if (!QFileInfo::exists(path))
        errors << "Missing image: " + file + " (<" + n.type + "> on slide line " +
                      QString::number(n.line) + ")";
    else if (!QImageReader(path).canRead())
        errors << "Cannot decode image: " + file;
}

const QRegularExpression &markdownImage() {
    static const QRegularExpression re(R"(!\[[^\]]*\]\((?:<([^>]+)>|([^\s)]+))\))");
    return re;
}

void validate(const QVector<Node> &nodes, const Ctx &c, const QStringList &userNames,
              QStringList &errors) {
    for (const Node &n : nodes) {
        if (n.type == "Text" && n.props.isEmpty())
            continue;
        const QString type = canonical(n.type);
        const auto spec = specs().constFind(type);
        const QString where = "<" + n.type + "> on slide line " + QString::number(n.line);
        if (spec == specs().cend()) {
            if (n.type == "Define")
                errors << "<Define> on slide line " + QString::number(n.line) +
                              " is inside another <Define>; give each its own block";
            else if (n.type == "Slot")
                errors << "<Slot> on slide line " + QString::number(n.line) +
                              " fills a template component; put it inside one";
            else
                errors << "Unknown component <" + n.type + "> on slide line " +
                              QString::number(n.line) +
                              suggestion(n.type, specs().keys() + aliases().keys() + userNames,
                                         "<%1>");
            continue;
        }
        for (auto it = n.props.cbegin(); it != n.props.cend(); ++it) {
            const QString key = it.key(), value = it.value().trimmed();
            if (!allowsProp(*spec, key)) {
                errors << where + " has no prop “" + key + "”" +
                              suggestion(key, spec->props + common);
                continue;
            }
            if (spec->choices.contains(key) && !value.isEmpty() &&
                !spec->choices.value(key).contains(value.toLower()))
                errors << where + ": " + key + "=\"" + value + "\" should be one of " +
                              spec->choices.value(key).join(", ");
            if (isColorProp(type, key))
                validateTone(n, key, value, c, errors);
            if ((key == "icon" && type != "List") || (key == "name" && type == "Icon"))
                validateIcon(n, value, errors);
            if (key == "icon" && type == "List" && value != "number" && value != "none")
                validateIcon(n, value, errors);
            if ((key == "src" && type != "Video") || key == "image" || key == "center-image")
                validateImage(n, value, c, errors);
            if ((key == "cols") && !value.isEmpty()) {
                bool ok = false;
                const int cols = value.toInt(&ok);
                if (!ok || cols < 1 || cols > 12)
                    errors << where + ": cols should be a whole number from 1 to 12";
            }
        }
        if (type == "Icon" && !n.has("name") && !n.templated)
            errors << where + " needs name=\"…\"";
        if (type == "Image" && !n.has("src") && !n.templated)
            errors << where + " needs src=\"…\"";
        if (type == "Stat" && !n.has("value") && !n.templated)
            errors << where + " needs value=\"…\"";
        if (type == "Person" && !n.has("name") && !n.templated)
            errors << where + " needs name=\"…\"";
        if (type == "QR" && !n.has("value") && !n.templated)
            errors << where + " needs value=\"…\"";
        if (type == "Video" && !n.has("src") && !n.templated)
            errors << where + " needs src=\"…\"";
        if (type == "Video" && n.has("src")) {
            const QString path = c.video(n.prop("src"));
            if (!QFileInfo::exists(path))
                errors << "Missing video: " + n.prop("src") + " (<Video> on slide line " +
                              QString::number(n.line) + ")";
        }
        if (type == "Video" && n.has("poster") && !QFileInfo::exists(c.deckImage(n.prop("poster"))))
            errors << "Missing poster: " + n.prop("poster") + " (<Video> on slide line " +
                          QString::number(n.line) +
                          "; posters come from the presentation's images/)";
        if (type == "Tree" || type == "Steps" || type == "Timeline" || type == "List")
            for (const Item &item : parseItems(n.text))
                validateImage(n, item.image, c, errors);
        if (type == "List" || type == "Tags" || type == "Steps" || type == "Timeline" ||
            type == "Cycle" || type == "Funnel" || type == "Venn")
            for (const Item &item : parseItems(n.text))
                validateIcon(n, item.icon, errors);
        if (type == "Logos" || type == "Collage") {
            auto m = markdownImage().globalMatch(n.text);
            while (m.hasNext()) {
                const auto match = m.next();
                validateImage(n,
                              match.captured(1).isEmpty() ? match.captured(2) : match.captured(1),
                              c, errors);
            }
        }
        for (const QString &problem : diagramProblems(n, c))
            errors << where + ": " + problem;
        validate(n.kids, c, userNames, errors);
    }
}

// ---------------------------------------------------------------------------
// Desugaring: every component becomes a few primitives the layout engine knows:
// Column, Row, Grid, Flow (containers that may be decorated), Text, Icon, Image,
// Pill, Logo, Divider, Spacer and the diagrams.

Node make(const QString &type, const QMap<QString, QString> &props = {}, const QString &text = {}) {
    Node n;
    n.type = type;
    n.props = props;
    n.text = text;
    return n;
}
void keep(Node &to, const Node &from, const QStringList &keys) {
    for (const QString &key : keys)
        if (from.has(key))
            to.props[key] = from.props.value(key);
}
QString escaped(QString text) { return text.replace("\n", "  \n"); }

void desugarAll(const QVector<Node> &in, QVector<Node> &out, Composition &slide);

Node desugarText(const Node &n) {
    Node t = make("Text", {}, n.text);
    keep(t, n,
         {"size", "tone", "align", "weight", "font", "uppercase", "tracking", "italic", "line",
          "rotate", "width", "height", "grow"});
    return t;
}

QVector<Node> kidsOf(const Node &n, Composition &slide) {
    QVector<Node> out;
    desugarAll(n.kids, out, slide);
    return out;
}

Node desugarCard(const Node &n, Composition &slide) {
    Node card = make("Column", {{"fill", n.prop("fill", "surface")},
                                {"radius", n.prop("radius", "theme")},
                                {"clip", "true"},
                                {"gap", "0"},
                                {"_card", "true"}});
    keep(card, n, {"border", "shadow", "width", "height", "grow", "angle"});
    if (!n.props.contains("shadow"))
        card.props["shadow"] = "auto";
    if (n.has("accent") && n.prop("accent") != "none") {
        card.props["bar"] = n.prop("accent");
        card.props["bar-tone"] = n.prop("tone", "accent");
    }
    const QString header = n.prop("header", "none");
    const QString align = n.prop("align", "start");
    if (header == "pill" && n.has("title")) {
        card.props["pill"] = n.prop("title");
        card.props["pill-tone"] = n.prop("tone", "gradient");
    }
    if (n.has("image")) {
        Node image = make("Image", {{"src", n.prop("image")}, {"fit", "cover"}});
        if (n.has("image-height"))
            image.props["height"] = n.prop("image-height");
        else
            image.props["aspect"] = "16:9";
        card.kids << image;
    }
    if (header == "tab" && n.has("title")) {
        Node tab =
            make("Column", {{"fill", n.prop("tone", "ink")}, {"pad", "20 28"}, {"align", align}});
        tab.kids << make("Text", {{"font", "@title"}, {"size", "md"}, {"align", align}},
                         escaped(n.prop("title")));
        card.kids << tab;
    }
    Node body = make("Column", {{"pad", n.prop("pad", "34")},
                                {"gap", n.prop("gap", "14")},
                                {"align", align},
                                {"justify", n.prop("justify", "start")},
                                {"_fill", "true"},
                                {"_text", n.prop("text-size", "sm")}});
    if (n.has("icon")) {
        Node icon = make("Icon", {{"name", n.prop("icon")},
                                  {"style", n.prop("icon-style", "plain")},
                                  {"tone", n.prop("tone", "accent")},
                                  {"size", n.prop("icon-size", "lg")}});
        body.kids << icon;
    }
    if (n.has("number"))
        body.kids << make(
            "Pill", {{"tone", n.prop("tone", "accent")}, {"variant", "square"}, {"size", "lg"}},
            n.prop("number"));
    if (n.has("title") && header == "none")
        body.kids << make("Text",
                          {{"font", "@title"},
                           {"size", "card"},
                           {"align", align},
                           {"_title", "true"},
                           {"tone", n.prop("title-tone")}},
                          escaped(n.prop("title")));
    body.kids += kidsOf(n, slide);
    card.kids << body;
    return card;
}

Node desugarStat(const Node &n, Composition &slide) {
    const QString size = n.prop("size", "md");
    const QString valueSize = size == "sm"   ? "stat-sm"
                              : size == "lg" ? "stat-lg"
                              : size == "xl" ? "stat-xl"
                                             : "stat";
    const QString align = n.prop("align", "start");
    const bool row = n.prop("layout") == "row";
    Node value = make("Row", {{"gap", "10"},
                              {"align", "center"},
                              {"justify", align == "center"                    ? "center"
                                          : align == "end" || align == "right" ? "end"
                                                                               : "start"}});
    if (n.has("trend") && n.prop("trend") != "flat") {
        const bool up = n.prop("trend") == "up";
        const bool good = up == (n.prop("good", "up") == "up");
        value.kids << make("Icon", {{"name", up ? "arrow-up" : "arrow-down"},
                                    {"tone", good ? "green" : "red"},
                                    {"size", size == "sm"   ? "md"
                                             : size == "md" ? "lg"
                                                            : "xl"},
                                    {"stroke", "2.5"}});
    }
    value.kids << make("Text",
                       {{"font", "@number"},
                        {"size", valueSize},
                        {"tone", n.prop("tone", "accent")},
                        {"line", "1.0"},
                        {"width", "auto"},
                        {"align", align}},
                       n.prop("value"));
    Node words = make("Column", {{"gap", "4"}, {"align", align}});
    if (n.has("label"))
        words.kids << make(
            "Text", {{"size", size == "sm" ? "xs" : "sm"}, {"weight", "700"}, {"align", align}},
            escaped(n.prop("label")));
    if (n.has("delta")) {
        const bool up = n.prop("trend", n.prop("delta").startsWith('-') ? "down" : "up") == "up";
        const bool good = up == (n.prop("good", "up") == "up");
        words.kids << make(
            "Text",
            {{"size", "xs"}, {"weight", "700"}, {"tone", good ? "green" : "red"}, {"align", align}},
            escaped(n.prop("delta")));
    }
    if (n.has("caption"))
        words.kids << make("Text", {{"size", "xs"}, {"tone", "muted"}, {"align", align}},
                           escaped(n.prop("caption")));
    words.kids += kidsOf(n, slide);
    Node stat = make(row ? "Row" : "Column",
                     {{"gap", row ? "20" : "8"}, {"align", row ? "center" : align}});
    keep(stat, n, {"width", "height", "grow"});
    if (n.has("icon"))
        stat.kids << make("Icon", {{"name", n.prop("icon")},
                                   {"style", n.prop("icon-style", "soft")},
                                   {"tone", n.prop("tone", "accent")},
                                   {"size", "md"}});
    if (row)
        value.props["width"] = "auto";
    stat.kids << value;
    if (!words.kids.isEmpty())
        stat.kids << words;
    return stat;
}

Node desugarPerson(const Node &n, Composition &slide) {
    const bool row = n.prop("layout") == "row";
    const QString align = n.prop("align", row ? "start" : "center");
    Node person = make(row ? "Row" : "Column",
                       {{"gap", row ? "22" : "10"}, {"align", row ? "center" : align}});
    keep(person, n, {"width", "height", "grow"});
    if (n.has("image")) {
        Node image = make("Image", {{"src", n.prop("image")},
                                    {"fit", "cover"},
                                    {"shape", n.prop("shape", "circle")},
                                    {"size", n.prop("size", row ? "120" : "180")},
                                    {"position", "top"}});
        if (n.has("ring"))
            image.props["ring"] = n.prop("ring");
        person.kids << image;
    }
    Node words = make("Column", {{"gap", "2"}, {"align", row ? "start" : align}});
    words.kids << make(
        "Text", {{"font", "@title"}, {"size", row ? "md" : "sm"}, {"align", row ? "left" : align}},
        escaped(n.prop("name")));
    if (n.has("role"))
        words.kids << make("Text",
                           {{"size", "xs"},
                            {"tone", n.prop("tone", "accent")},
                            {"weight", "600"},
                            {"align", row ? "left" : align}},
                           escaped(n.prop("role")));
    if (n.has("caption"))
        words.kids << make("Text",
                           {{"size", "xs"}, {"tone", "muted"}, {"align", row ? "left" : align}},
                           escaped(n.prop("caption")));
    for (Node kid : kidsOf(n, slide)) {
        if (kid.type == "Text" && !kid.has("size"))
            kid.props["size"] = "xs";
        words.kids << kid;
    }
    person.kids << words;
    return person;
}

Node desugarList(const Node &n) {
    const QString style = n.prop("icon-style", n.prop("icon") == "number" ? "number" : "box");
    const int cols = qMax(1, n.prop("cols", "1").toInt());
    Node list = make(cols > 1 ? "Grid" : "Column",
                     {{"gap", n.prop("gap", "16")}, {"cols", QString::number(cols)}});
    if (cols > 1)
        list.props["balance"] = "false";
    keep(list, n, {"width", "height", "grow"});
    if (n.flag("divided"))
        list.props["divided"] = "true";
    if (n.flag("connected"))
        list.props["_connect"] = n.prop("tone", "ink");
    int number = 0;
    for (const Item &item : parseItems(n.text)) {
        Node row = make("Row", {{"gap", "16"}, {"align", "start"}});
        ++number;
        const QString icon = item.icon.isEmpty() ? n.prop("icon", "check") : item.icon;
        Node marker;
        if (style == "number")
            marker = make("Pill",
                          {{"tone", n.prop("tone", "accent")},
                           {"variant", "circle"},
                           {"size", n.prop("size", "sm")}},
                          QString::number(number));
        else if (style == "dot")
            marker = make("Icon", {{"name", "_dot"},
                                   {"tone", n.prop("tone", "accent")},
                                   {"size", n.prop("size", "sm")}});
        else
            marker = make("Icon", {{"name", icon},
                                   {"style", style == "box" ? "square" : style},
                                   {"tone", n.prop("tone", "accent")},
                                   {"size", n.prop("size", "sm")},
                                   {"_marker", "true"}});
        row.kids << marker;
        QString markdown =
            item.title.isEmpty()
                ? item.text
                : "**" + item.title + "**" +
                      (item.text.isEmpty() ? "" : (item.textBelow ? "  \n" : " ") + item.text);
        row.kids << make("Text", {{"size", n.prop("size", "")}}, markdown);
        list.kids << row;
    }
    return list;
}

Node desugarQuote(const Node &n, Composition &slide) {
    const QString align = n.prop("align", "start");
    Node quote = make("Column", {{"gap", "18"}, {"align", align}});
    keep(quote, n, {"width", "height", "grow"});
    quote.kids << make(
        "Icon",
        {{"name", "quote"}, {"tone", n.prop("tone", "accent")}, {"size", "xl"}, {"stroke", "1.5"}});
    for (Node kid : kidsOf(n, slide)) {
        if (kid.type == "Text") {
            if (!kid.has("size"))
                kid.props["size"] = n.prop("size", "lg");
            if (!kid.has("font"))
                kid.props["font"] = "@heading";
            if (!kid.has("weight"))
                kid.props["weight"] = "500";
        }
        quote.kids << kid;
    }
    if (n.has("by") || n.has("image")) {
        Node who = make("Row", {{"gap", "16"},
                                {"align", "center"},
                                {"justify", align == "center" ? "center" : "start"},
                                {"width", "auto"}});
        if (n.has("image"))
            who.kids << make(
                "Image",
                {{"src", n.prop("image")}, {"shape", "circle"}, {"fit", "cover"}, {"size", "72"}});
        Node words = make("Column", {{"gap", "0"}, {"width", "auto"}});
        if (n.has("by"))
            words.kids << make("Text", {{"size", "sm"}, {"weight", "700"}}, "— " + n.prop("by"));
        if (n.has("role"))
            words.kids << make("Text", {{"size", "xs"}, {"tone", "muted"}}, n.prop("role"));
        who.kids << words;
        quote.kids << who;
    }
    return quote;
}

Node desugarCallout(const Node &n, Composition &slide) {
    Node bar = make("Row", {{"fill", n.prop("fill", "ink")},
                            {"radius", n.prop("radius", "theme")},
                            {"pad", n.prop("pad", "22 30")},
                            {"gap", "20"},
                            {"align", "center"},
                            {"_text", n.prop("size", "sm")}});
    keep(bar, n, {"width", "height", "grow", "angle"});
    bar.kids << make("Icon", {{"name", n.prop("icon", "circle-alert")},
                              {"tone", n.prop("tone", "ink")},
                              {"size", "md"}});
    QString markdown;
    if (n.has("title"))
        markdown = "**" + n.prop("title") + "** ";
    QVector<Node> kids = kidsOf(n, slide);
    if (kids.size() == 1 && kids[0].type == "Text") {
        Node text = kids[0];
        text.text = markdown + text.text;
        bar.kids << text;
    } else {
        Node column = make("Column", {{"gap", "8"}});
        if (!markdown.isEmpty())
            column.kids << make("Text", {}, markdown);
        column.kids += kids;
        bar.kids << column;
    }
    return bar;
}

Node desugarSplit(const Node &n, Composition &slide) {
    const bool vertical = n.prop("direction") == "column";
    Node split = make(vertical ? "Column" : "Row", {{"gap", n.prop("gap", "0")},
                                                    {"align", "stretch"},
                                                    {"justify", "stretch"},
                                                    {"_split", "true"}});
    const QStringList ratio =
        n.prop("ratio").split(QRegularExpression("[:/ ,]+"), Qt::SkipEmptyParts);
    QVector<Node> kids = kidsOf(n, slide);
    for (int i = 0; i < kids.size(); ++i) {
        Node panel = kids[i];
        if (!panel.props.contains("_panel")) {
            Node wrap =
                make("Column",
                     {{"pad", "72"}, {"justify", "center"}, {"gap", "24"}, {"_panel", "true"}});
            wrap.kids << panel;
            panel = wrap;
        }
        if (i < ratio.size())
            panel.props[vertical ? "grow" : "width"] = ratio[i];
        else if (!panel.has(vertical ? "grow" : "width"))
            panel.props[vertical ? "grow" : "width"] = "1";
        split.kids << panel;
    }
    return split;
}

Node desugarNode(const Node &n, Composition &slide) {
    const QString type = canonical(n.type);
    if (type == "Text")
        return desugarText(n);
    if (type == "Row" || type == "Column" || type == "Grid") {
        Node c = make(type, n.props);
        c.kids = kidsOf(n, slide);
        return c;
    }
    if (type == "Panel") {
        Node c = make("Column", n.props);
        c.props["_panel"] = "true";
        if (!n.props.contains("pad"))
            c.props["pad"] = "72";
        if (!n.props.contains("justify"))
            c.props["justify"] = "center";
        if (!n.props.contains("gap"))
            c.props["gap"] = "24";
        c.kids = kidsOf(n, slide);
        return c;
    }
    if (type == "Split")
        return desugarSplit(n, slide);
    if (type == "Card")
        return desugarCard(n, slide);
    if (type == "Stat")
        return desugarStat(n, slide);
    if (type == "Person")
        return desugarPerson(n, slide);
    if (type == "List")
        return desugarList(n);
    if (type == "Quote")
        return desugarQuote(n, slide);
    if (type == "Callout")
        return desugarCallout(n, slide);
    if (type == "Eyebrow") {
        Node t = make("Text",
                      {{"size", n.prop("size", "eyebrow")},
                       {"font", "@eyebrow"},
                       {"uppercase", "true"},
                       {"tracking", "12"},
                       {"weight", "@eyebrow"},
                       {"tone", n.prop("tone", "accent")},
                       {"_eyebrow", "true"}},
                      n.text);
        keep(t, n, {"align", "width", "grow"});
        return t;
    }
    if (type == "Pill") {
        Node pill = make("Pill", n.props, plainText(n.text));
        return pill;
    }
    if (type == "Tags") {
        Node flow = make("Flow", {{"gap", n.prop("gap", "12")}});
        keep(flow, n, {"align", "width", "height", "grow"});
        for (const Item &item : parseItems(n.text)) {
            Node pill =
                make("Pill",
                     {{"tone", n.prop("tone", "accent")},
                      {"variant", n.prop("variant", "soft")},
                      {"size", n.prop("size", "sm")}},
                     plainText(item.title.isEmpty()
                                   ? item.text
                                   : item.title + (item.text.isEmpty() ? "" : " " + item.text)));
            if (!item.icon.isEmpty())
                pill.props["icon"] = item.icon;
            flow.kids << pill;
        }
        return flow;
    }
    if (type == "Table") {
        Node t = make("Text", {{"_table", "true"}}, n.text);
        keep(t, n, {"header", "zebra", "size", "highlight", "width", "height", "grow"});
        return t;
    }
    if (type == "Logos") {
        Node grid = make(
            "Grid",
            {{"cols", n.prop("cols", "4")}, {"gap", n.prop("gap", "36")}, {"align", "center"}});
        keep(grid, n, {"width", "height", "grow"});
        auto m = markdownImage().globalMatch(n.text);
        while (m.hasNext()) {
            const auto match = m.next();
            grid.kids << make("Logo", {{"src", match.captured(1).isEmpty() ? match.captured(2)
                                                                           : match.captured(1)},
                                       {"tone", n.prop("tone", "original")},
                                       {"size", n.prop("size", "80")}});
        }
        return grid;
    }
    // Icon, Image, Divider, Spacer and diagrams are primitives already.
    Node leaf = make(type, n.props, n.text);
    leaf.kids = kidsOf(n, slide);
    return leaf;
}

void desugarAll(const QVector<Node> &in, QVector<Node> &out, Composition &slide) {
    for (const Node &n : in) {
        const QString type = canonical(n.type);
        if (!specs().contains(type) && type != "Text")
            continue; // Reported by validation.
        if (type == "Background") {
            slide.backgrounds << n;
            continue;
        }
        if (type == "Place") {
            Node place = make("Column", n.props);
            place.props["_place"] = "true";
            place.kids = kidsOf(n, slide);
            slide.overlays << place;
            continue;
        }
        Node d = desugarNode(n, slide);
        // Sizing props every component takes travel to whatever it became.
        for (const QString &key : common)
            if (n.has(key) && !d.props.contains(key))
                d.props[key] = n.props.value(key);
        // Empty text from an unset template prop simply disappears.
        if ((d.type == "Text" || d.type == "Pill") && d.text.trimmed().isEmpty())
            continue;
        if ((d.type == "Image" || d.type == "Logo") && !d.has("src") && n.templated)
            continue;
        out << d;
    }
}

// ---------------------------------------------------------------------------

QString slideFrame(const QString &source, const QVariantMap &palette) {
    const QString own = slideProperty(source, "frame");
    if (!own.isEmpty())
        return own == "none" ? QString() : own;
    return palette.value("frame").toString();
}

// A slide built on a template component may ask for its own frame, or none.
QString componentFrame(const QString &source, const Definitions &defs, bool *found) {
    *found = false;
    QStringList ignored;
    for (const Node &n : parse(source, ignored)) {
        if (n.type == "Text" && n.text.trimmed().isEmpty())
            continue;
        const auto def = defs.byName.constFind(n.type);
        if (def != defs.byName.cend() && def->hasFrame) {
            *found = true;
            return def->frame == "none" ? QString() : def->frame;
        }
        break;
    }
    return {};
}

Composition compose(const QString &source, const QVariantMap &palette, Ctx &c, bool check = true) {
    Composition slide;
    const Definitions defs = definitions(palette.value("template_id").toString());
    QString body = source;
    QString frame = slideFrame(source, palette);
    bool own = false;
    const QString chosen = componentFrame(source, defs, &own);
    if (own && slideProperty(source, "frame").isEmpty())
        frame = chosen;
    if (!frame.isEmpty() && defs.byName.contains(frame)) {
        // The frame receives the whole slide as {children}; comments stay out of it.
        body = "<" + frame + ">\n" + blankComments(source) + "\n</" + frame + ">";
    }
    QVector<Node> nodes = parse(body, slide.errors);
    if (!frame.isEmpty() && defs.byName.contains(frame)) {
        // The slide's own lines start after the wrapper's opening line.
        for (Node &n : nodes)
            if (n.textLine > 0)
                n.textLine -= 1;
    } else if (!frame.isEmpty())
        slide.errors << "The frame <" + frame + "> is not defined by the template";
    expand(nodes, defs, palette, slide.errors, 0);
    if (check) {
        // The slide's own definitions are checked here, where their lines are known.
        if (source.contains("<Define"))
            slide.errors += readDefinitions(source, true).errors;
        validate(nodes, c, defs.byName.keys(), slide.errors);
    }
    desugarAll(nodes, slide.flow, slide);
    slide.justify = slideProperty(source, "justify");
    if (slide.justify.isEmpty())
        slide.justify = "auto";
    slide.bleed = slide.flow.size() == 1 && slide.flow[0].has("_split");
    return slide;
}

Ctx context(const QString &base, const QVariantMap &palette) {
    Ctx c;
    c.palette = palette;
    c.base = base;
    c.templateDirs = palette.value("template_dirs").toString().split('\n', Qt::SkipEmptyParts);
    c.lightSlide = luminance(c.color("background")) > 0.5;
    return c;
}

QMutex fitMutex;
QCache<QString, qreal> fits(2048);

} // namespace

// ---------------------------------------------------------------------------
// Markdown data helpers

Table parseTable(const QString &markdown) {
    Table table;
    static const QRegularExpression separator("^\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?$");
    for (const QString &raw : markdown.split('\n')) {
        const QString line = raw.trimmed();
        if (!line.startsWith('|') && !line.contains('|'))
            continue;
        if (separator.match(line).hasMatch())
            continue;
        QString row = line;
        if (row.startsWith('|'))
            row.remove(0, 1);
        if (row.endsWith('|') && !row.endsWith("\\|"))
            row.chop(1);
        QStringList cells;
        QString cell;
        for (int i = 0; i < row.size(); ++i) {
            if (row[i] == '\\' && i + 1 < row.size() && row[i + 1] == '|') {
                cell += '|';
                ++i;
            } else if (row[i] == '|') {
                cells << cell.trimmed();
                cell.clear();
            } else
                cell += row[i];
        }
        cells << cell.trimmed();
        if (table.header.isEmpty())
            table.header = cells;
        else
            table.rows << cells;
    }
    return table;
}

QVector<Item> parseItems(const QString &markdown) {
    QVector<Item> items;
    static const QRegularExpression bullet("^(\\s*)(?:[-*+]|\\d+[.)])\\s+(.*)$");
    static const QRegularExpression icon("^:([a-z0-9-]+):\\s*");
    static const QRegularExpression picture("^!\\[[^\\]]*\\]\\((?:<([^>]+)>|([^\\s)]+))\\)\\s*");
    static const QRegularExpression title("^\\*\\*(.+?)\\*\\*\\s*(?:[—–:-]\\s*)?");
    for (const QString &line : markdown.split('\n')) {
        const auto m = bullet.match(line);
        if (m.hasMatch()) {
            Item item;
            item.level = m.captured(1).size() / 2;
            QString rest = m.captured(2).trimmed();
            if (const auto i = icon.match(rest); i.hasMatch()) {
                item.icon = i.captured(1);
                rest = rest.mid(i.capturedEnd());
            }
            if (const auto i = picture.match(rest); i.hasMatch()) {
                item.image = i.captured(1).isEmpty() ? i.captured(2) : i.captured(1);
                rest = rest.mid(i.capturedEnd());
            }
            if (const auto t = title.match(rest); t.hasMatch()) {
                item.title = t.captured(1).trimmed();
                rest = rest.mid(t.capturedEnd());
            }
            item.text = rest.trimmed();
            items << item;
        } else if (!items.isEmpty() && !line.trimmed().isEmpty()) {
            if (items.last().text.isEmpty())
                items.last().textBelow = true;
            items.last().text += (items.last().text.isEmpty() ? "" : "  \n") + line.trimmed();
        }
    }
    return items;
}

double parseNumber(const QString &text, bool *ok) {
    QString t = plainText(text).trimmed().toLower();
    t.remove(QRegularExpression("[$€£¥,%~<>≈+]"));
    t.replace(QString::fromUtf8("−"), "-");
    // A scale is a k, m, mm, b or bn on its own ("$1.2M", "40 k"), or a word for one.
    // Any other trailing word is a unit: "3 months" is 3, and "12kg" is 12.
    static const QRegularExpression number("^(-?[0-9]*\\.?[0-9]+)\\s*(?:(k|mm|m|bn|b|thousand|"
                                           "million|billion)(?![a-z]))?\\s*([a-z][a-z ./]*)?$");
    const auto m = number.match(t.simplified());
    if (!m.hasMatch()) {
        if (ok)
            *ok = false;
        return 0;
    }
    const QString unit = m.captured(2);
    double multiplier = 1;
    if (unit == "k" || unit == "thousand")
        multiplier = 1e3;
    else if (unit == "m" || unit == "mm" || unit == "million")
        multiplier = 1e6;
    else if (unit == "b" || unit == "bn" || unit == "billion")
        multiplier = 1e9;
    bool converted = false;
    const double value = m.captured(1).toDouble(&converted) * multiplier;
    if (ok)
        *ok = converted;
    return value;
}

QString plainText(const QString &markdown) {
    QString t = markdown;
    static const QRegularExpression link("!?\\[([^\\]]*)\\]\\([^)]*\\)");
    t.replace(link, "\\1");
    static const QRegularExpression marks("(\\*\\*|__|\\*|`|~~)");
    t.remove(marks);
    static const QRegularExpression underline("(^|\\W)_(.+?)_(?=\\W|$)");
    t.replace(underline, "\\1\\2");
    return t.trimmed();
}

// ---------------------------------------------------------------------------
// Public interface

bool hasComponents(const QString &source) {
    static const QRegularExpression tag("^\\s*<(/?)([A-Z][A-Za-z0-9]*)(?=[\\s/>]|$)(.*)$",
                                        QRegularExpression::MultilineOption);
    // So does `# Title {align=center}`, which would otherwise show its braces.
    static const QRegularExpression attributed("^\\s{0,3}#{1,6}\\s.*\\{[^{}]*=[^{}]*\\}\\s*$",
                                               QRegularExpression::MultilineOption);
    static const QRegularExpression prop("^\\s+[A-Za-z][\\w:-]*(\\s*=|\\s*/?>|\\s+[A-Za-z])");
    if (!source.contains('<') && !source.contains('{'))
        return false;
    const QString visible = outsideCode(blankComments(source), true);
    if (attributed.match(visible).hasMatch())
        return true;
    // Prose such as "<Enter> submits the form" is not a component: a tag is a known name, closes
    // itself, takes props, or is closed later on the slide.
    for (auto it = tag.globalMatch(visible); it.hasNext();) {
        const auto m = it.next();
        const QString name = m.captured(2), rest = m.captured(3);
        if (specs().contains(canonical(name)) || name == "Define" || name == "Slot" ||
            rest.contains("/>") || prop.match(rest).hasMatch() ||
            (m.captured(1).isEmpty() && visible.contains("</" + name + ">")))
            return true;
    }
    return false;
}

bool isComposedSlide(const QString &source, const QVariantMap &palette) {
    if (hasComponents(source))
        return true;
    const QString frame = slideFrame(source, palette);
    if (frame.isEmpty() || palette.value("template_id").toString().isEmpty())
        return false;
    // Plain text slides take the frame; picture, gallery and cloud slides keep their layouts.
    const QString layout = slideProperty(source, "layout");
    if (!layout.isEmpty())
        return false;
    return !markdownImage().match(outsideCode(blankComments(source), true)).hasMatch();
}

QStringList composedProblems(const QString &source, const QString &base,
                             const QVariantMap &palette) {
    Ctx c = context(base, palette);
    Composition slide = compose(source, palette, c);
    QStringList problems = slide.errors;
    problems.removeDuplicates();
    return problems;
}

namespace {
// Lays the slide out at its fit factor: one factor for the whole slide keeps headings, body
// and gaps in proportion. Dense slides shrink; sparse ones grow, short of the edges.
// The pictures and videos a composed slide shows; their sizes shape its layout.
void collectAssets(const QVector<Node> &nodes, const Ctx &c, QStringList &out) {
    static const QRegularExpression image(R"(!\[[^\]]*\]\((?:<([^>]+)>|([^\s)]+))\))");
    for (const Node &n : nodes) {
        for (const QString &key : {"src", "image", "center-image", "poster"}) {
            if (!n.has(key))
                continue;
            const QString file = n.prop(key);
            out << (n.type == "Video" ? (key == "src" ? c.video(file) : c.deckImage(file))
                                      : c.asset(file));
        }
        if (n.text.contains("!["))
            for (auto it = image.globalMatch(n.text); it.hasNext();) {
                const auto m = it.next();
                out << c.asset(m.captured(1).isEmpty() ? m.captured(2) : m.captured(1));
            }
        collectAssets(n.kids, c, out);
    }
}
QStringList assetsOf(const Composition &slide, const Ctx &c) {
    QStringList out;
    collectAssets(slide.backgrounds, c, out);
    collectAssets(slide.flow, c, out);
    collectAssets(slide.overlays, c, out);
    out.removeDuplicates();
    return out;
}

qreal fitSlide(Composition &slide, Ctx &c, const QString &source, const QString &base,
               const QVariantMap &palette) {
    QByteArray identity;
    {
        QDataStream stream(&identity, QIODevice::WriteOnly);
        stream << source << base << palette;
        // A replaced picture can change the layout as much as an edit can.
        for (const QString &path : assetsOf(slide, c)) {
            const QFileInfo file(path);
            stream << file.lastModified().toMSecsSinceEpoch() << file.size();
        }
    }
    const QString key =
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex());
    qreal scale = -1;
    {
        QMutexLocker lock(&fitMutex);
        if (const qreal *cached = fits.object(key))
            scale = *cached;
    }
    if (scale < 0) {
        const QString mode = slideProperty(source, "fit").isEmpty()
                                 ? palette.value("fit").toString()
                                 : slideProperty(source, "fit");
        const qreal most =
            mode == "shrink" ? 1.0 : qBound(1.0, palette.value("fit_max", 1.4).toDouble(), 2.0);
        auto attempt = [&](qreal s) {
            Composition trial = slide;
            Ctx t = c;
            t.s = s;
            t.p = nullptr;
            t.slack = s > 1 ? 0.9 : 1;
            return layoutComposition(trial, t);
        };
        qreal low = 0.45, high = 1;
        if (attempt(1)) {
            low = 1;
            high = most;
        }
        if (high > low && attempt(high))
            low = high;
        for (int i = 0; i < 8 && high - low > 0.005; ++i) {
            const qreal mid = (low + high) / 2;
            (attempt(mid) ? low : high) = mid;
        }
        scale = low;
        QMutexLocker lock(&fitMutex);
        fits.insert(key, new qreal(scale));
    }
    c.s = scale;
    c.slack = scale > 1 ? 0.9 : 1;
    layoutComposition(slide, c);
    return scale;
}
const Node *findType(const QVector<Node> &nodes, const QString &type) {
    for (const Node &n : nodes) {
        if (n.type == type)
            return &n;
        if (const Node *found = findType(n.kids, type))
            return found;
    }
    return nullptr;
}
} // namespace

void paintComposed(QPainter *p, const QString &source, const QString &base,
                   const QVariantMap &palette, QString *warning, QStringList *problems) {
    Ctx c = context(base, palette);
    c.p = p;
    Composition slide = compose(source, palette, c);
    if (problems) {
        *problems = slide.errors;
        problems->removeDuplicates();
    }
    const qreal scale = fitSlide(slide, c, source, base, palette);
    paintComposition(slide, c);
    if (warning) {
        if (c.smallest < 16)
            *warning = "Text fits below 16px on a 1080p slide; say less or split the slide";
        else if (scale < 0.75)
            *warning = QString("Content shrinks to %1% to fit; say less or split the slide")
                           .arg(qRound(scale * 100));
    }
}

QStringList componentAssets(const QString &source, const QString &base,
                            const QVariantMap &palette) {
    static QMutex mutex;
    static QCache<QString, QStringList> cache(512);
    QByteArray identity;
    {
        QDataStream stream(&identity, QIODevice::WriteOnly);
        stream << source << base << palette;
    }
    const QString key =
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex());
    {
        QMutexLocker lock(&mutex);
        if (const QStringList *cached = cache.object(key))
            return *cached;
    }
    Ctx c = context(base, palette);
    const QStringList assets = assetsOf(compose(source, palette, c, false), c);
    QMutexLocker lock(&mutex);
    cache.insert(key, new QStringList(assets));
    return assets;
}

QMap<QString, QString> componentVideo(const QString &source) {
    // Look inside template components too, whose content is parsed when they expand.
    std::function<QMap<QString, QString>(const QString &, int)> search = [&](const QString &text,
                                                                             int depth) {
        QStringList errors;
        const QVector<Node> nodes = parse(text, errors);
        if (const Node *video = findType(nodes, "Video"))
            return video->props;
        std::function<QMap<QString, QString>(const QVector<Node> &)> inner =
            [&](const QVector<Node> &list) {
                for (const Node &n : list) {
                    if (!specs().contains(canonical(n.type)) && depth < 8 &&
                        n.text.contains("<Video")) {
                        const auto found = search(n.text, depth + 1);
                        if (!found.isEmpty())
                            return found;
                    }
                    const auto found = inner(n.kids);
                    if (!found.isEmpty())
                        return found;
                }
                return QMap<QString, QString>();
            };
        return inner(nodes);
    };
    return source.contains("<Video") ? search(source, 0) : QMap<QString, QString>();
}

QRectF componentMediaBox(const QString &source, const QString &base, const QVariantMap &palette) {
    Ctx c = context(base, palette);
    Composition slide = compose(source, palette, c);
    fitSlide(slide, c, source, base, palette);
    const Node *video = findType(slide.flow, "Video");
    if (!video)
        video = findType(slide.overlays, "Video");
    return video ? video->rect : QRectF();
}

QStringList templateCandidates(const QString &name, const QString &base) {
    QStringList candidates;
    QString path = name.trimmed();
    if (path.isEmpty())
        return candidates;
    if (path == "~" || path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    // A template is a presentation, or a folder holding one (or a template.md).
    if (path.endsWith(".md"))
        return {QDir::cleanPath(QDir(base).absoluteFilePath(path))};
    QStringList folders;
    if (!path.contains('/'))
        folders << QDir(base).absoluteFilePath("templates/" + path);
    folders << QDir::cleanPath(QDir(base).absoluteFilePath(path));
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!path.contains('/') && !data.isEmpty())
        folders << data + "/hype/templates/" + path;
    for (const QString &folder : folders)
        candidates << folder + "/template.md" << folder + "/presentation.md";
    // The templates Hype comes with come last, so any of yours can take their names.
    if (!path.contains('/'))
        candidates << ":/templates/" + path + "/presentation.md";
    return candidates;
}

TemplatePack loadTemplate(const QString &name, const QString &base, const QString &presentation) {
    TemplatePack pack;
    // A template may name a template of its own; each one's settings and definitions win
    // over those it builds on.
    QStringList headers, definitions, seen;
    if (!presentation.isEmpty())
        seen << QFileInfo(presentation).absoluteFilePath()
             << QFileInfo(presentation).canonicalFilePath();
    QString next = name.trimmed(), from = base;
    while (!next.isEmpty()) {
        const QStringList candidates = templateCandidates(next, from);
        pack.watched += candidates;
        const auto found =
            std::find_if(candidates.cbegin(), candidates.cend(),
                         [](const QString &path) { return QFileInfo(path).isFile(); });
        if (found == candidates.cend()) {
            pack.error +=
                "Template “" + next + "” not found; looked in " + candidates.join(", ") + "\n";
            break;
        }
        const QFileInfo info(*found);
        // A library link reads its pictures, fonts and templates from beside the file it follows.
        const QString folder =
            info.canonicalPath().isEmpty() ? info.absolutePath() : info.canonicalPath();
        if (seen.contains(info.absoluteFilePath()) || seen.contains(info.canonicalFilePath())) {
            pack.error += "Template “" + next + "” builds on itself\n";
            break;
        }
        if (seen.size() > 8) {
            pack.error += "Templates build on each other too deeply\n";
            break;
        }
        seen << info.absoluteFilePath() << info.canonicalFilePath();
        QFile file(*found);
        if (!file.open(QIODevice::ReadOnly)) {
            pack.error += *found + ": " + file.errorString() + "\n";
            break;
        }
        const QString text = QString::fromUtf8(file.readAll());
        const auto parsed = parseDeck(text);
        // Everything after the front matter may hold definitions.
        const QString body = parsed.slides.isEmpty() ? text : text.mid(parsed.slides.first().start);
        if (pack.path.isEmpty())
            pack.path = info.absoluteFilePath();
        if (pack.theme.isEmpty())
            pack.theme = scalar(parsed.header, "theme");
        pack.directories << folder;
        headers << parsed.header;
        definitions.prepend(body);
        const QString label = headers.size() > 1 ? info.absoluteFilePath() + ": " : QString();
        for (const QString &error : readDefinitions(body).errors)
            pack.error += label + error + "\n";
        next = scalar(parsed.header, "template");
        from = folder;
    }
    pack.header = headers.join('\n'); // The nearest first, so its settings are the ones read.
    pack.definitions = definitions.join("\n\n");
    pack.error = pack.error.trimmed();
    return pack;
}

QString withoutDefinitions(const QString &source) { return blankDefinitions(source); }

QString definitionsIn(const QString &text) {
    if (!text.contains("<Define"))
        return {};
    const QStringList lines = blankComments(text).split('\n');
    QStringList blocks;
    for (const auto &[first, last] : definitionBlocks(lines))
        blocks << lines.mid(first, last - first + 1).join('\n');
    return blocks.join("\n\n");
}

QString rememberTemplate(const QString &text) {
    const QString id = QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
    QMutexLocker lock(&registryMutex);
    if (!registry.contains(id)) {
        Definitions defs = readDefinitions(text);
        defs.pages = text.contains("{page");
        registry.insert(id, defs);
        registryOrder << id;
        // Each edit of a template registers it anew; keep the recent ones.
        while (registryOrder.size() > 64)
            registry.remove(registryOrder.takeFirst());
    }
    return id;
}

bool templateShowsPages(const QString &id) { return definitions(id).pages; }

bool isBuiltInComponent(const QString &name) { return specs().contains(canonical(name)); }

QString componentGuide() {
    QFile guide(":/components.md");
    return guide.open(QIODevice::ReadOnly) ? QString::fromUtf8(guide.readAll()) : QString();
}
