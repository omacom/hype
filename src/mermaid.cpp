#include "mermaid.h"
#include <QCache>
#include <QFontMetricsF>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QtMath>
#include <algorithm>
#include <climits>
#include <functional>
#include <tuple>
#include <limits>
#include <memory>
#include <numeric>

namespace {
enum class Shape { Rect, Round, Stadium, Subroutine, Cylinder, Circle, DoubleCircle, Diamond,
                   Hexagon, Lean, LeanAlt, Trapezoid, TrapezoidAlt, Flag };
enum class Line { Solid, Dotted, Thick, Invisible };
enum class Head { None, Arrow, Circle, Cross };
enum class Dir { TB, BT, LR, RL };

// Layout units: node text is set at 28px, then the finished diagram scales to its slide area.
constexpr qreal textSize = 28, labelSize = 24, titleSize = 24, nodeGap = 56, rankGap = 72,
                clusterPad = 24, wrapWidth = 420, maxScale = 1.5, loopReach = 48;

struct Style {
    QColor fill, stroke, text;
    qreal width = 0;
    bool dashed = false;
    void merge(const Style &other) {
        if (other.fill.isValid()) fill = other.fill;
        if (other.stroke.isValid()) stroke = other.stroke;
        if (other.text.isValid()) text = other.text;
        if (other.width > 0) width = other.width;
        dashed = dashed || other.dashed;
    }
};
struct End {
    bool cluster = false;
    int index = -1;
    bool operator==(const End &other) const { return cluster == other.cluster && index == other.index; }
};
struct Node {
    QString id, label;
    Shape shape = Shape::Rect;
    int cluster = -1, host = -1; // host: the closed subgraph, or diagram, whose layout places it
    QStringList classes, lines;
    Style style;
    QSizeF size;
    QPointF local;
    QRectF rect;
};
struct Cluster {
    QString id, title;
    int parent = -1, depth = 0, host = -1;
    Dir dir = Dir::TB;
    bool dirSet = false, open = false; // open: linked from outside to a node inside
    QRectF box;                         // an open subgraph's box, within its host's layout
    Style style;
    QStringList lines;
    qreal titleHeight = 0, titleWidth = 0;
    QSizeF size;
    QPointF local, content;
    QRectF rect;
};
struct Edge {
    QString from, to, label;
    End a, b, liftedA, liftedB;
    Line line = Line::Solid;
    Head head = Head::Arrow, tail = Head::None;
    int length = 1, level = -1, labelAt = -1;
    bool drawable = false, loop = false;
    QStringList lines;
    QSizeF labelSize;
    QVector<QPointF> middle;
    QPainterPath path;
    QPointF labelPos, headTip, headDir, tailTip, tailDir;
};
struct Diagram {
    QString error;
    Dir dir = Dir::TB;
    QVector<Node> nodes;
    QVector<Cluster> clusters;
    QVector<Edge> edges;
    QHash<QString, int> nodeIndex;
    QRectF bounds;
};

QFont diagramFont(const QString &family, qreal size, bool bold = false) {
    QFont font(family);
    font.setPixelSize(qRound(size));
    font.setHintingPreference(QFont::PreferNoHinting);
    if (bold)
        font.setWeight(QFont::DemiBold);
    return font;
}
bool parseDir(const QString &text, Dir *dir) {
    const QString t = text.trimmed().toUpper();
    if (t == "TB" || t == "TD")
        *dir = Dir::TB;
    else if (t == "BT")
        *dir = Dir::BT;
    else if (t == "LR")
        *dir = Dir::LR;
    else if (t == "RL")
        *dir = Dir::RL;
    else
        return false;
    return true;
}
QString cleanLabel(QString text) {
    text = text.trimmed();
    if (text.size() >= 2 && text.startsWith('"') && text.endsWith('"'))
        text = text.mid(1, text.size() - 2);
    if (text.size() >= 2 && text.startsWith('`') && text.endsWith('`')) {
        // Markdown strings: keep the words, drop the emphasis markers.
        text = text.mid(1, text.size() - 2);
        static const QRegularExpression strong(R"(\*\*(.+?)\*\*|__(.+?)__)"), em(R"(\*(.+?)\*|_(.+?)_)");
        text.replace(strong, "\\1\\2").replace(em, "\\1\\2");
    }
    static const QRegularExpression br("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption),
        tags("</?[a-zA-Z][^>]*>"), icons("\\bfa[bklrs]?:fa-[\\w-]+\\s*"), entity("(?:&#?|#)(\\w+);");
    text.replace(br, "\n").remove(tags).remove(icons);
    static constexpr const char *named[][2]{{"quot", "\""}, {"amp", "&"}, {"lt", "<"},
                                            {"gt", ">"},     {"apos", "'"}, {"nbsp", " "}};
    QString out;
    int last = 0;
    for (auto it = entity.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        bool numeric;
        const uint code = m.captured(1).toUInt(&numeric);
        QString replacement = m.captured(0);
        if (numeric && code > 0 && code <= 0x10FFFF) {
            const char32_t c = code;
            replacement = QString::fromUcs4(&c, 1);
        } else
            for (const auto &entry : named)
                if (m.captured(1) == QLatin1StringView(entry[0]))
                    replacement = QLatin1StringView(entry[1]);
        out += text.mid(last, m.capturedStart() - last) + replacement;
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out.trimmed();
}
Style parseStyle(const QString &spec) {
    Style style;
    for (const QString &part : spec.split(',')) {
        const QString key = part.section(':', 0, 0).trimmed().toLower();
        const QString value = part.section(':', 1).remove("!important").remove(';').trimmed();
        if (key == "fill")
            style.fill = QColor(value);
        else if (key == "stroke")
            style.stroke = QColor(value);
        else if (key == "color")
            style.text = QColor(value);
        else if (key == "stroke-width")
            style.width = QString(value).remove("px").toDouble();
        else if (key == "stroke-dasharray")
            style.dashed = true;
    }
    return style;
}
Head headFrom(const QString &mark) {
    return mark == ">" ? Head::Arrow : mark == "o" ? Head::Circle : mark == "x" ? Head::Cross : Head::None;
}
void skipSpace(const QString &s, int &p) {
    while (p < s.size() && s[p].isSpace())
        ++p;
}

struct Statement {
    QString text;
    int line;
};
QVector<Statement> statements(const QString &source) {
    QVector<Statement> result;
    const QStringList lines = QString(source).replace("\r\n", "\n").split('\n');
    int i = 0;
    while (i < lines.size() && lines[i].trimmed().isEmpty())
        ++i;
    if (i < lines.size() && lines[i].trimmed() == "---") {
        // Front matter holds a title and configuration; neither changes the drawing.
        for (++i; i < lines.size() && lines[i].trimmed() != "---"; ++i) {}
        ++i;
    }
    for (; i < lines.size(); ++i) {
        const QString &line = lines[i];
        if (line.trimmed().startsWith("%%"))
            continue;
        int depth = 0, start = 0;
        bool quoted = false;
        for (int c = 0; c <= line.size(); ++c) {
            const QChar ch = c < line.size() ? line[c] : QChar(';');
            if (c < line.size() && ch == '"')
                quoted = !quoted;
            else if (c < line.size() && !quoted && QStringLiteral("[({").contains(ch))
                ++depth;
            else if (c < line.size() && !quoted && QStringLiteral("])}").contains(ch))
                depth = qMax(0, depth - 1);
            else if (ch == ';' && (c == line.size() || (!quoted && depth == 0))) {
                const QString piece = line.mid(start, c - start).trimmed();
                if (!piece.isEmpty())
                    result.append({piece, i + 1});
                start = c + 1;
            }
        }
    }
    return result;
}

class Parser {
  public:
    explicit Parser(Diagram &diagram) : d(diagram) {}
    void parse(const QString &source);

  private:
    Diagram &d;
    int line = 1;
    QVector<int> stack;
    QHash<QString, Style> classDefs, styles;
    QVector<QPair<QString, QString>> assigned;

    bool fail(const QString &message) {
        if (d.error.isEmpty())
            d.error = QString("Mermaid line %1: %2").arg(line).arg(message);
        return false;
    }
    bool statement(const QString &text);
    bool chain(const QString &s);
    bool group(const QString &s, int &p, QStringList &ids);
    bool node(const QString &s, int &p, QString &id);
    bool link(const QString &s, int &p, Edge &edge);
    bool pipe(const QString &s, int &p, Edge &edge);
    int ensure(const QString &id);
    void finish();
};

void Parser::parse(const QString &source) {
    const auto list = statements(source);
    if (list.isEmpty()) {
        fail("The diagram is empty");
        return;
    }
    line = list[0].line;
    static const QRegularExpression header("^(flowchart|graph)(?:-elk)?(?:\\s+(\\S+))?$");
    const auto m = header.match(list[0].text);
    if (!m.hasMatch()) {
        const QString type = list[0].text.section(QRegularExpression("\\s"), 0, 0);
        static constexpr const char *known[]{
            "sequenceDiagram", "classDiagram", "stateDiagram", "stateDiagram-v2", "erDiagram",
            "journey", "gantt", "pie", "quadrantChart", "requirementDiagram", "gitGraph",
            "C4Context", "C4Container", "C4Component", "C4Dynamic", "C4Deployment", "mindmap",
            "timeline", "sankey-beta", "xychart-beta", "block-beta", "packet-beta", "kanban",
            "architecture-beta", "radar-beta"};
        const bool diagram = std::any_of(std::begin(known), std::end(known),
                                         [&](const char *name) { return type == QLatin1StringView(name); });
        fail(diagram ? QString("Hype draws Mermaid flowcharts; %1 isn't supported yet").arg(type)
                                  : QString("Start the diagram with “flowchart TD” or “flowchart LR”"));
        return;
    }
    if (!m.captured(2).isEmpty() && !parseDir(m.captured(2), &d.dir)) {
        fail("Unknown direction “" + m.captured(2) + "”; use TB, BT, LR, or RL");
        return;
    }
    for (int i = 1; i < list.size(); ++i) {
        line = list[i].line;
        if (!statement(list[i].text))
            return;
    }
    if (!stack.isEmpty()) {
        fail(QString("subgraph “%1” needs an “end”").arg(d.clusters[stack.last()].title));
        return;
    }
    finish();
}

bool Parser::statement(const QString &text) {
    // The first word decides what kind of statement this is.
    const qsizetype split = std::find_if(text.begin(), text.end(), [](QChar c) { return c.isSpace(); }) - text.begin();
    const QStringView keyword = QStringView(text).left(split);
    const QString rest = text.mid(split).trimmed();
    if (text == "end") {
        if (stack.isEmpty())
            return fail("“end” without a subgraph");
        stack.removeLast();
        return true;
    }
    if (keyword == u"subgraph") {
        const QString &spec = rest;
        static const QRegularExpression named(R"(^([^\s\["]+)\s*\[(.*)\]$)");
        Cluster &cluster = d.clusters.emplaceBack();
        if (const auto m = named.match(spec); m.hasMatch()) {
            cluster.id = m.captured(1);
            cluster.title = cleanLabel(m.captured(2));
        } else {
            cluster.title = cleanLabel(spec);
            cluster.id = spec.startsWith('"') ? cluster.title : spec;
        }
        if (cluster.id.isEmpty())
            cluster.id = QString("subgraph-%1").arg(d.clusters.size() - 1);
        cluster.parent = stack.isEmpty() ? -1 : stack.last();
        stack.append(d.clusters.size() - 1);
        return true;
    }
    if (keyword == u"direction") {
        Dir dir;
        if (!parseDir(rest, &dir))
            return fail("Unknown direction; use TB, BT, LR, or RL");
        if (stack.isEmpty())
            d.dir = dir;
        else {
            d.clusters[stack.last()].dir = dir;
            d.clusters[stack.last()].dirSet = true;
        }
        return true;
    }
    if (keyword == u"classDef") {
        const QString &spec = rest;
        for (const QString &name : spec.section(' ', 0, 0).split(',', Qt::SkipEmptyParts))
            classDefs[name.trimmed()].merge(parseStyle(spec.section(' ', 1)));
        return true;
    }
    if (keyword == u"class") {
        const QString &spec = rest;
        for (const QString &id : spec.section(' ', 0, 0).split(',', Qt::SkipEmptyParts))
            assigned.append({id.trimmed(), spec.section(' ', 1).trimmed()});
        return true;
    }
    if (keyword == u"style") {
        const QString &spec = rest;
        styles[spec.section(' ', 0, 0)].merge(parseStyle(spec.section(' ', 1)));
        return true;
    }
    for (const QStringView ignored : {u"linkStyle", u"click", u"accTitle", u"accDescr", u"title"})
        if (keyword == ignored || (keyword.startsWith(ignored) && keyword.mid(ignored.size()).startsWith(u':')))
            return true;
    return chain(text);
}

bool Parser::chain(const QString &s) {
    int p = 0;
    QStringList previous;
    if (!group(s, p, previous))
        return false;
    for (skipSpace(s, p); p < s.size(); skipSpace(s, p)) {
        Edge edge;
        const int at = p;
        if (!link(s, p, edge))
            return fail(QString("Expected an arrow like --> before “%1”").arg(s.mid(at, 24).trimmed()));
        QStringList next;
        if (!group(s, p, next))
            return false;
        for (const QString &from : previous)
            for (const QString &to : next) {
                edge.from = from;
                edge.to = to;
                d.edges.append(edge);
            }
        previous = next;
    }
    return true;
}

bool Parser::group(const QString &s, int &p, QStringList &ids) {
    for (;;) {
        QString id;
        if (!node(s, p, id))
            return false;
        ids.append(id);
        skipSpace(s, p);
        if (p >= s.size() || s[p] != '&')
            return true;
        ++p;
    }
}

bool Parser::node(const QString &s, int &p, QString &id) {
    skipSpace(s, p);
    const int start = p;
    static const QString stops = "[](){}<>|&;:\"'@,=~";
    while (p < s.size()) {
        const QChar c = s[p];
        if (c.isSpace() || stops.contains(c))
            break;
        if (c == '-' && p + 1 < s.size() && QStringLiteral("-.>").contains(s[p + 1]))
            break;
        ++p;
    }
    id = s.mid(start, p - start);
    if (id.isEmpty())
        return fail(p < s.size() ? QString("Expected a node name before “%1”").arg(s.mid(p, 24).trimmed())
                                 : QString("Expected a node name"));
    const int index = ensure(id);
    struct Delimiter {
        const char *open, *close;
        Shape shape;
    };
    static constexpr Delimiter delimiters[]{
        {"(((", ")))", Shape::DoubleCircle}, {"((", "))", Shape::Circle}, {"([", "])", Shape::Stadium},
        {"[[", "]]", Shape::Subroutine},     {"[(", ")]", Shape::Cylinder}, {"{{", "}}", Shape::Hexagon},
        {"[/", "/]", Shape::Lean},           {"[/", "\\]", Shape::Trapezoid},
        {"[\\", "\\]", Shape::LeanAlt},      {"[\\", "/]", Shape::TrapezoidAlt},
        {"(", ")", Shape::Round},            {"[", "]", Shape::Rect},
        {"{", "}", Shape::Diamond},          {">", "]", Shape::Flag}};
    int best = -1, bestClose = -1;
    for (int i = 0; i < int(std::size(delimiters)); ++i) {
        const auto &delimiter = delimiters[i];
        // Once an opening matches, only its alternative closings compete.
        if (best >= 0 && qstrcmp(delimiter.open, delimiters[best].open))
            break;
        if (!QStringView(s).mid(p).startsWith(QLatin1StringView(delimiter.open)))
            continue;
        const int from = p + qstrlen(delimiter.open);
        int close = -1;
        if (from < s.size() && s[from] == '"') {
            const int quote = s.indexOf('"', from + 1);
            if (quote < 0)
                return fail("A node label is missing its closing quote");
            if (QStringView(s).mid(quote + 1).startsWith(QLatin1StringView(delimiter.close)))
                close = quote + 1;
        } else
            close = s.indexOf(QLatin1StringView(delimiter.close), from);
        if (close >= 0 && (best < 0 || close < bestClose)) {
            best = i;
            bestClose = close;
        }
    }
    if (best >= 0) {
        const auto &delimiter = delimiters[best];
        const int from = p + qstrlen(delimiter.open);
        d.nodes[index].label = cleanLabel(s.mid(from, bestClose - from));
        d.nodes[index].shape = delimiter.shape;
        p = bestClose + qstrlen(delimiter.close);
    } else if (p < s.size() && QStringLiteral("[({").contains(s[p]))
        return fail(QString("The shape of “%1” is missing its closing bracket").arg(id));
    if (QStringView(s).mid(p).startsWith(QLatin1String("@{"))) {
        const int close = s.indexOf('}', p);
        if (close < 0)
            return fail(QString("The @{ … } of “%1” is missing its closing brace").arg(id));
        struct Name {
            const char *name;
            Shape shape;
        };
        static constexpr Name shapes[]{
            {"rect", Shape::Rect},          {"rectangle", Shape::Rect},      {"proc", Shape::Rect},
            {"process", Shape::Rect},       {"rounded", Shape::Round},       {"event", Shape::Round},
            {"stadium", Shape::Stadium},    {"pill", Shape::Stadium},        {"terminal", Shape::Stadium},
            {"subproc", Shape::Subroutine}, {"subprocess", Shape::Subroutine}, {"subroutine", Shape::Subroutine},
            {"fr-rect", Shape::Subroutine}, {"framed-rectangle", Shape::Subroutine},
            {"cyl", Shape::Cylinder},       {"cylinder", Shape::Cylinder},   {"database", Shape::Cylinder},
            {"db", Shape::Cylinder},        {"circle", Shape::Circle},       {"circ", Shape::Circle},
            {"dbl-circ", Shape::DoubleCircle}, {"double-circle", Shape::DoubleCircle},
            {"diam", Shape::Diamond},       {"diamond", Shape::Diamond},     {"decision", Shape::Diamond},
            {"question", Shape::Diamond},   {"hex", Shape::Hexagon},         {"hexagon", Shape::Hexagon},
            {"prepare", Shape::Hexagon},    {"lean-r", Shape::Lean},         {"lean-right", Shape::Lean},
            {"in-out", Shape::Lean},        {"lean-l", Shape::LeanAlt},      {"lean-left", Shape::LeanAlt},
            {"out-in", Shape::LeanAlt},     {"trap-b", Shape::Trapezoid},    {"trapezoid", Shape::Trapezoid},
            {"trapezoid-bottom", Shape::Trapezoid}, {"priority", Shape::Trapezoid},
            {"trap-t", Shape::TrapezoidAlt}, {"trapezoid-top", Shape::TrapezoidAlt},
            {"manual", Shape::TrapezoidAlt}, {"odd", Shape::Flag}};
        static const QRegularExpression pair(R"re((\w+)\s*:\s*("(?:[^"\\]|\\.)*"|[^,]+))re");
        const QString body = s.mid(p + 2, close - p - 2);
        for (auto it = pair.globalMatch(body); it.hasNext();) {
            const auto m = it.next();
            const QString value = m.captured(2).trimmed();
            if (m.captured(1) == "shape") {
                const auto shape = std::find_if(std::begin(shapes), std::end(shapes),
                                                [&](const Name &name) { return value == QLatin1StringView(name.name); });
                if (shape == std::end(shapes))
                    return fail(QString("Hype can't draw the “%1” shape yet").arg(value));
                d.nodes[index].shape = shape->shape;
            } else if (m.captured(1) == "label")
                d.nodes[index].label = cleanLabel(value);
        }
        p = close + 1;
    }
    if (QStringView(s).mid(p).startsWith(QLatin1String(":::"))) {
        p += 3;
        const int from = p;
        while (p < s.size() && (s[p].isLetterOrNumber() || s[p] == '_' ||
                                (s[p] == '-' && !(p + 1 < s.size() && QStringLiteral("-.>").contains(s[p + 1])))))
            ++p;
        assigned.append({id, s.mid(from, p - from)});
    }
    return true;
}

bool Parser::link(const QString &s, int &p, Edge &edge) {
    auto at = [&](int i) { return i < s.size() ? s[i] : QChar(); };
    auto word = [&](int i) { return at(i).isLetterOrNumber() || at(i) == '_'; };
    int q = p;
    if (at(q) == '<') {
        edge.tail = Head::Arrow;
        ++q;
    } else if ((at(q) == 'o' || at(q) == 'x') && (at(q + 1) == '-' || at(q + 1) == '=')) {
        edge.tail = at(q) == 'o' ? Head::Circle : Head::Cross;
        ++q;
    }
    // The closing half of a labeled link, like the “-->” in “-- yes -->”.
    auto labeled = [&](const QRegularExpression &end, int minimum) {
        for (auto it = end.globalMatch(s, q); it.hasNext();) {
            const auto m = it.next();
            if (m.captured(2).isEmpty() && m.captured(1).size() < minimum)
                continue;
            edge.label = cleanLabel(s.mid(q, m.capturedStart() - q));
            if (edge.label.isEmpty())
                return false;
            edge.head = headFrom(m.captured(2));
            edge.length = 1;
            q = m.capturedEnd();
            return true;
        }
        return false;
    };
    if (at(q) == '~') {
        int n = 0;
        while (at(q) == '~')
            ++q, ++n;
        if (n < 3)
            return false;
        edge.line = Line::Invisible;
        edge.head = edge.tail = Head::None;
        edge.length = n - 2;
    } else if (at(q) == '-' && at(q + 1) == '.') {
        edge.line = Line::Dotted;
        ++q;
        int dots = 0;
        while (at(q) == '.')
            ++q, ++dots;
        if (at(q) == '-') {
            ++q;
            edge.head = Head::None;
            if (at(q) == '>' || ((at(q) == 'o' || at(q) == 'x') && !word(q + 1)))
                edge.head = headFrom(at(q++));
            edge.length = dots;
        } else if (dots == 1 && at(q).isSpace()) {
            static const QRegularExpression end(R"((\.+)-(>|[ox](?!\w))?)");
            if (!labeled(end, 1))
                return false;
        } else
            return false;
    } else if (at(q) == '-' || at(q) == '=') {
        const QChar kind = at(q);
        edge.line = kind == '-' ? Line::Solid : Line::Thick;
        int n = 0;
        while (at(q) == kind)
            ++q, ++n;
        if (n < 2)
            return false;
        if (at(q) == '>' || ((at(q) == 'o' || at(q) == 'x') && !word(q + 1))) {
            edge.head = headFrom(at(q++));
            edge.length = n - 1;
        } else if (n >= 3) {
            edge.head = Head::None;
            edge.length = n - 2;
        } else if (at(q).isSpace()) {
            static const QRegularExpression solid(R"((-{2,})(>|[ox](?!\w))?)"), thick(R"((={2,})(>|[ox](?!\w))?)");
            if (!labeled(kind == '-' ? solid : thick, 3))
                return false;
        } else
            return false;
    } else
        return false;
    p = q;
    return pipe(s, p, edge);
}

bool Parser::pipe(const QString &s, int &p, Edge &edge) {
    skipSpace(s, p);
    if (p < s.size() && s[p] == '|') {
        const int close = s.indexOf('|', p + 1);
        if (close < 0)
            return fail("A |label| is missing its closing bar");
        edge.label = cleanLabel(s.mid(p + 1, close - p - 1));
        p = close + 1;
    }
    return true;
}

int Parser::ensure(const QString &id) {
    int index = d.nodeIndex.value(id, -1);
    if (index < 0) {
        Node &node = d.nodes.emplaceBack();
        node.id = node.label = id;
        index = d.nodes.size() - 1;
        d.nodeIndex.insert(id, index);
    }
    // A node belongs to the first subgraph that mentions it, even after a top-level mention.
    if (!stack.isEmpty() && d.nodes[index].cluster < 0)
        d.nodes[index].cluster = stack.last();
    return index;
}

void Parser::finish() {
    // A subgraph's id used in a link means the subgraph itself.
    QHash<QString, int> clusterIndex;
    for (int i = 0; i < d.clusters.size(); ++i)
        if (!clusterIndex.contains(d.clusters[i].id))
            clusterIndex.insert(d.clusters[i].id, i);
    d.nodes.removeIf([&](const Node &node) { return clusterIndex.contains(node.id); });
    QHash<QString, int> index;
    for (int i = 0; i < d.nodes.size(); ++i)
        index.insert(d.nodes[i].id, i);
    d.nodeIndex = index;
    auto resolve = [&](const QString &id) {
        End end;
        if (clusterIndex.contains(id)) {
            end.cluster = true;
            end.index = clusterIndex.value(id);
        } else
            end.index = index.value(id, -1);
        return end;
    };
    for (auto &edge : d.edges) {
        edge.a = resolve(edge.from);
        edge.b = resolve(edge.to);
    }
    for (auto &cluster : d.clusters) {
        const Cluster *parent = cluster.parent < 0 ? nullptr : &d.clusters[cluster.parent];
        if (!cluster.dirSet)
            cluster.dir = parent ? parent->dir : d.dir;
        cluster.depth = parent ? parent->depth + 1 : 0;
        cluster.style = styles.value(cluster.id);
    }
    for (const auto &[id, name] : assigned)
        if (index.contains(id))
            d.nodes[index.value(id)].classes.append(name);
    for (auto &node : d.nodes) {
        Style style = classDefs.value("default");
        for (const QString &name : node.classes)
            style.merge(classDefs.value(name));
        style.merge(styles.value(node.id));
        node.style = style;
    }
    if (d.nodes.isEmpty() && d.clusters.isEmpty())
        fail("The flowchart has no nodes");
}

QStringList wrap(const QString &text, const QFontMetricsF &metrics, qreal width) {
    QStringList lines;
    for (const QString &paragraph : text.split('\n')) {
        QString current;
        for (const QString &word : paragraph.split(' ', Qt::SkipEmptyParts)) {
            const QString candidate = current.isEmpty() ? word : current + ' ' + word;
            if (!current.isEmpty() && metrics.horizontalAdvance(candidate) > width) {
                lines.append(current);
                current = word;
            } else
                current = candidate;
        }
        lines.append(current);
    }
    return lines;
}
QSizeF blockSize(const QStringList &lines, const QFontMetricsF &metrics) {
    qreal width = 0;
    for (const QString &line : lines)
        width = qMax(width, metrics.horizontalAdvance(line));
    return {width, lines.size() * metrics.lineSpacing()};
}
qreal cylinderCap(qreal width) { return qMin(width * 0.08 + 4, 18.0); }

void measure(Diagram &d, const QString &family) {
    const QFontMetricsF text(diagramFont(family, textSize)), label(diagramFont(family, labelSize)),
        title(diagramFont(family, titleSize, true));
    const qreal padX = 26, padY = 16;
    for (auto &node : d.nodes) {
        node.lines = wrap(node.label, text, wrapWidth);
        const QSizeF t = blockSize(node.lines, text);
        const qreal w = t.width() + 2 * padX, h = t.height() + 2 * padY;
        switch (node.shape) {
        case Shape::Stadium: node.size = {w + h / 2, h}; break;
        case Shape::Subroutine: node.size = {w + 24, h}; break;
        case Shape::Cylinder: node.size = {w, h + 2 * cylinderCap(w)}; break;
        case Shape::Circle:
        case Shape::DoubleCircle: {
            // The text block's diagonal fits inside the circle.
            qreal diameter = qSqrt(t.width() * t.width() + t.height() * t.height()) + 28;
            if (node.shape == Shape::DoubleCircle)
                diameter += 14;
            node.size = {diameter, diameter};
            break;
        }
        case Shape::Diamond: {
            // A rhombus holds the text block when (w/2)/a + (h/2)/b <= 1.
            const qreal b = t.height() / 2 + 30, a = (t.width() / 2) / (1 - t.height() / 2 / b) + 12;
            node.size = {qMax(2 * a, 2 * b), 2 * b};
            break;
        }
        case Shape::Hexagon:
        case Shape::Lean:
        case Shape::LeanAlt:
        case Shape::Trapezoid:
        case Shape::TrapezoidAlt: node.size = {w + h / 2, h}; break;
        case Shape::Flag: node.size = {w + h / 4, h}; break;
        default: node.size = {w, h};
        }
    }
    for (auto &edge : d.edges)
        if (!edge.label.isEmpty()) {
            edge.lines = wrap(edge.label, label, wrapWidth * 0.75);
            edge.labelSize = blockSize(edge.lines, label) + QSizeF(20, 10);
        }
    for (auto &cluster : d.clusters) {
        cluster.lines = cluster.title.isEmpty() ? QStringList() : wrap(cluster.title, title, wrapWidth * 1.5);
        const QSizeF t = blockSize(cluster.lines, title);
        cluster.titleWidth = t.width();
        cluster.titleHeight = cluster.lines.isEmpty() ? 0 : t.height() + 8;
    }
}

// One layered (Sugiyama-style) layout for what a closed subgraph, or the diagram, holds.
// Closed subgraphs inside were laid out first and take part as single boxes. Open subgraphs,
// whose inner nodes are linked from outside, are laid out here along with everything else:
// their members stay side by side in every rank, between border items that become the box's sides.
QSizeF layoutLevel(Diagram &d, int level) {
    const Dir dir = level < 0 ? d.dir : d.clusters[level].dir;
    const bool across = dir == Dir::LR || dir == Dir::RL;
    struct Item {
        int node = -1, cluster = -1, rank = 0, border = 0, group = -1; // border: -1 before, 1 after
        bool label = false;
        qreal w = 0, h = 0;
        QVector<int> chain; // open subgraphs holding the item, outermost first
    };
    struct Neighbor {
        int item;
        qreal weight;
    };
    QVector<Item> items;
    QVector<QVector<Neighbor>> preds, succs;
    QVector<int> nodeItem(d.nodes.size(), -1), clusterItem(d.clusters.size(), -1);
    auto chainFrom = [&](int c) {
        QVector<int> chain;
        for (; c != level; c = d.clusters[c].parent)
            chain.prepend(c);
        return chain;
    };
    auto add = [&](Item item, QSizeF size) {
        item.w = across ? size.height() : size.width();
        item.h = across ? size.width() : size.height();
        items.append(item);
        preds.append(QVector<Neighbor>());
        succs.append(QVector<Neighbor>());
        return int(items.size() - 1);
    };
    for (int i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].host == level) {
            Item item;
            item.node = i;
            item.chain = chainFrom(d.nodes[i].cluster);
            nodeItem[i] = add(item, d.nodes[i].size);
        }
    // A self-loop bulges out across the flow, which is along the rank; keep neighbors clear of it.
    for (const auto &edge : d.edges)
        if (edge.loop && edge.level == level) {
            const QSizeF label = edge.labelSize;
            items[nodeItem[edge.a.index]].w += 2 * (loopReach + (across ? label.height() : label.width()));
        }
    for (int i = 0; i < d.clusters.size(); ++i)
        if (!d.clusters[i].open && d.clusters[i].host == level) {
            Item item;
            item.cluster = i;
            item.chain = chainFrom(d.clusters[i].parent);
            clusterItem[i] = add(item, d.clusters[i].size);
        }
    const int real = items.size();
    if (!real)
        return {};
    auto itemOf = [&](End end) { return end.cluster ? clusterItem[end.index] : nodeItem[end.index]; };

    struct Link {
        int u, v, edge, minlen;
        bool reversed = false;
        QVector<int> chain;
        int from() const { return reversed ? v : u; }
        int to() const { return reversed ? u : v; }
    };
    QVector<Link> links;
    bool labels = false;
    for (int i = 0; i < d.edges.size(); ++i) {
        const auto &edge = d.edges[i];
        if (edge.level != level || !edge.drawable || edge.loop)
            continue;
        links.append({itemOf(edge.liftedA), itemOf(edge.liftedB), i, qMax(1, edge.length), false, {}});
        labels = labels || !edge.label.isEmpty();
    }
    // As in dagre, labels sit on their own rank halfway along a link that spans twice as far.
    const qreal gap = labels ? rankGap / 2 : rankGap;
    if (labels)
        for (auto &link : links)
            link.minlen *= 2;

    // Break cycles: links that close a loop in a depth-first walk point backwards for ranking.
    QVector<QVector<int>> outgoing(real);
    QVector<int> incoming(real, 0), state(real, 0);
    for (int k = 0; k < links.size(); ++k) {
        outgoing[links[k].u].append(k);
        ++incoming[links[k].v];
    }
    auto visit = [&](auto &self, int u) -> void {
        state[u] = 1;
        for (int k : outgoing[u]) {
            const int v = links[k].v;
            if (state[v] == 1)
                links[k].reversed = true;
            else if (!state[v])
                self(self, v);
        }
        state[u] = 2;
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int u = 0; u < real; ++u)
            if (!state[u] && (pass || !incoming[u]))
                visit(visit, u);

    // Rank by longest path, then pull each source down next to its first successor.
    QVector<QVector<int>> in(real), out(real);
    for (int k = 0; k < links.size(); ++k) {
        in[links[k].to()].append(k);
        out[links[k].from()].append(k);
    }
    QVector<int> order, degree(real);
    for (int u = 0; u < real; ++u)
        if (!(degree[u] = in[u].size()))
            order.append(u);
    for (int i = 0; i < order.size(); ++i)
        for (int k : out[order[i]]) {
            const int v = links[k].to();
            items[v].rank = qMax(items[v].rank, items[order[i]].rank + links[k].minlen);
            if (!--degree[v])
                order.append(v);
        }
    for (int i = order.size() - 1; i >= 0; --i) {
        const int u = order[i];
        if (!in[u].isEmpty() || out[u].isEmpty())
            continue;
        int rank = INT_MAX;
        for (int k : out[u])
            rank = qMin(rank, items[links[k].to()].rank - links[k].minlen);
        items[u].rank = rank;
    }
    int lowest = INT_MAX, ranks = 0;
    for (int i = 0; i < real; ++i)
        lowest = qMin(lowest, items[i].rank);
    for (int i = 0; i < real; ++i)
        ranks = qMax(ranks, (items[i].rank -= lowest) + 1);

    // Long links pass through one dummy per rank; a label dummy carries the label's size. Dummies
    // stay inside only the open subgraphs holding both ends, so links from outside route around.
    auto dummy = [&](int i) { return items[i].node < 0 && items[i].cluster < 0; };
    auto connect = [&](int a, int b, qreal weight = 0) {
        if (!weight)
            weight = dummy(a) && dummy(b) ? 8 : dummy(a) || dummy(b) ? 2 : 1;
        succs[a].append({b, weight});
        preds[b].append({a, weight});
    };
    for (auto &link : links) {
        const int a = link.from(), b = link.to();
        const auto &edge = d.edges[link.edge];
        const int labelRank = edge.label.isEmpty() ? -1 : items[a].rank + (items[b].rank - items[a].rank) / 2;
        QVector<int> shared;
        for (int i = 0; i < qMin(items[a].chain.size(), items[b].chain.size()) &&
                        items[a].chain[i] == items[b].chain[i]; ++i)
            shared.append(items[a].chain[i]);
        int previous = a;
        for (int r = items[a].rank + 1; r < items[b].rank; ++r) {
            Item item;
            item.rank = r;
            item.chain = shared;
            QSizeF size;
            if (r == labelRank) {
                item.label = true;
                size = edge.labelSize;
            }
            const int index = add(item, size);
            link.chain.append(index);
            connect(previous, index);
            previous = index;
        }
        connect(previous, b);
    }
    // Each open subgraph gets a border item on either side in every rank it spans, chained
    // tightly from rank to rank so its sides run straight.
    QVector<int> groups;
    QVector<QPair<int, int>> span(d.clusters.size(), {INT_MAX, -1});
    for (int i = 0; i < items.size(); ++i)
        for (int g : items[i].chain)
            span[g] = {qMin(span[g].first, items[i].rank), qMax(span[g].second, items[i].rank)};
    QVector<QVector<int>> starts(d.clusters.size()), ends(d.clusters.size());
    for (int g = 0; g < d.clusters.size(); ++g) {
        if (span[g].second < 0)
            continue;
        groups.append(g);
        for (int r = span[g].first; r <= span[g].second; ++r)
            for (int side : {-1, 1}) {
                Item item;
                item.rank = r;
                item.border = side;
                item.group = g;
                item.chain = chainFrom(g);
                auto &list = side < 0 ? starts[g] : ends[g];
                const int index = add(item, {});
                if (!list.isEmpty())
                    connect(list.last(), index, 10);
                list.append(index);
            }
    }
    const int n = items.size();

    // Order each rank: depth-first placement, then barycenter sweeps, keeping the fewest crossings.
    // Arranging sorts whole subgraphs as blocks, then what's inside them, so members stay together.
    QVector<qreal> key(n, 0);
    auto arrange = [&](auto &self, const QVector<int> &members, int depth) -> QVector<int> {
        struct Unit {
            int group;
            QVector<int> members;
            qreal key;
        };
        QVector<Unit> units;
        for (int m : members) {
            if (items[m].chain.size() > depth) {
                const int g = items[m].chain[depth];
                auto unit = std::find_if(units.begin(), units.end(), [&](const Unit &u) { return u.group == g; });
                if (unit == units.end())
                    unit = units.insert(units.end(), {g, {}, 0});
                unit->members.append(m);
            } else
                units.append({-1, {m}, items[m].border ? items[m].border * 1e18 : key[m]});
        }
        for (auto &unit : units) {
            if (unit.group < 0)
                continue;
            qreal sum = 0;
            int count = 0;
            for (int m : unit.members)
                if (!(items[m].border && items[m].group == unit.group)) {
                    sum += key[m];
                    ++count;
                }
            unit.key = count ? sum / count : key[unit.members.first()];
        }
        std::stable_sort(units.begin(), units.end(), [](const Unit &a, const Unit &b) { return a.key < b.key; });
        QVector<int> ordered;
        for (const auto &unit : units)
            ordered += unit.group >= 0 ? self(self, unit.members, depth + 1) : unit.members;
        return ordered;
    };
    QVector<QVector<int>> layers(ranks);
    QVector<bool> seen(n, false);
    auto place = [&](auto &self, int i) -> void {
        if (seen[i])
            return;
        seen[i] = true;
        layers[items[i].rank].append(i);
        for (const auto &next : succs[i])
            self(self, next.item);
    };
    for (int i = 0; i < real; ++i)
        if (preds[i].isEmpty())
            place(place, i);
    for (int i = 0; i < n; ++i)
        place(place, i);
    QVector<int> position(n);
    auto index = [&](const QVector<QVector<int>> &all) {
        for (const auto &layer : all)
            for (int i = 0; i < layer.size(); ++i)
                position[layer[i]] = i;
    };
    for (auto &layer : layers) {
        for (int i = 0; i < layer.size(); ++i)
            key[layer[i]] = i;
        layer = arrange(arrange, layer, 0);
    }
    auto crossings = [&]() {
        int total = 0;
        for (int r = 0; r + 1 < ranks; ++r) {
            QVector<QPair<int, int>> pairs;
            for (int a : layers[r])
                for (const auto &next : succs[a])
                    pairs.append({position[a], position[next.item]});
            for (int i = 0; i < pairs.size(); ++i)
                for (int j = i + 1; j < pairs.size(); ++j)
                    if ((pairs[i].first - pairs[j].first) * (pairs[i].second - pairs[j].second) < 0)
                        ++total;
        }
        return total;
    };
    index(layers);
    auto best = layers;
    int fewest = crossings();
    for (int iteration = 0; iteration < 24 && fewest > 0; ++iteration) {
        const bool down = iteration % 2 == 0;
        for (int step = 1; step < ranks; ++step) {
            auto &layer = layers[down ? step : ranks - 1 - step];
            for (int i = 0; i < layer.size(); ++i) {
                const auto &neighbors = down ? preds[layer[i]] : succs[layer[i]];
                qreal sum = 0;
                for (const auto &neighbor : neighbors)
                    sum += position[neighbor.item];
                key[layer[i]] = neighbors.isEmpty() ? qreal(i) : sum / neighbors.size();
            }
            layer = arrange(arrange, layer, 0);
            for (int i = 0; i < layer.size(); ++i)
                position[layer[i]] = i;
        }
        if (const int count = crossings(); count < fewest) {
            fewest = count;
            best = layers;
        }
    }
    layers = best;
    index(layers);

    // Place along each rank: pull every item toward its neighbors, then resolve overlaps exactly
    // with isotonic regression, so ranks stay ordered and spaced.
    QVector<qreal> x(n, 0);
    auto separation = [&](int a, int b) {
        const Item &left = items[a], &right = items[b];
        qreal space;
        if (left.border < 0 && right.chain.contains(left.group))
            // Across the flow, a subgraph's title sits on this side.
            space = clusterPad + (across ? d.clusters[left.group].titleHeight : 0);
        else if (right.border > 0 && left.chain.contains(right.group))
            space = clusterPad;
        else
            space = dummy(a) || dummy(b) ? nodeGap / 2 : nodeGap;
        return (left.w + right.w) / 2 + space;
    };
    for (const auto &layer : layers)
        for (int i = 1; i < layer.size(); ++i)
            x[layer[i]] = x[layer[i - 1]] + separation(layer[i - 1], layer[i]);
    bool pinBorders = false;
    auto relax = [&](int r, bool usePreds, bool useSuccs) {
        const auto &layer = layers[r];
        struct Block {
            qreal sum, weight;
            int count;
        };
        QVector<Block> blocks;
        QVector<qreal> offset(layer.size(), 0);
        for (int i = 0; i < layer.size(); ++i) {
            if (i)
                offset[i] = offset[i - 1] + separation(layer[i - 1], layer[i]);
            qreal sum = 0, weight = 0;
            for (const auto &neighbor : usePreds ? preds[layer[i]] : QVector<Neighbor>()) {
                sum += neighbor.weight * x[neighbor.item];
                weight += neighbor.weight;
            }
            for (const auto &neighbor : useSuccs ? succs[layer[i]] : QVector<Neighbor>()) {
                sum += neighbor.weight * x[neighbor.item];
                weight += neighbor.weight;
            }
            qreal want = weight > 0 ? sum / weight : x[layer[i]];
            weight = weight > 0 ? weight : 0.01;
            if (pinBorders && items[layer[i]].border) {
                want = x[layer[i]];
                weight = 1e6;
            }
            blocks.append({weight * (want - offset[i]), weight, 1});
            while (blocks.size() > 1) {
                auto &previous = blocks[blocks.size() - 2];
                const auto &last = blocks.last();
                if (previous.sum / previous.weight <= last.sum / last.weight)
                    break;
                previous.sum += last.sum;
                previous.weight += last.weight;
                previous.count += last.count;
                blocks.removeLast();
            }
        }
        int i = 0;
        for (const auto &block : blocks)
            for (int c = 0; c < block.count; ++c, ++i)
                x[layer[i]] = block.sum / block.weight + offset[i];
    };
    for (int round = 0; round < 8; ++round) {
        for (int r = 1; r < ranks; ++r)
            relax(r, true, false);
        for (int r = ranks - 2; r >= 0; --r)
            relax(r, false, true);
    }
    for (int r = 0; r < ranks; ++r)
        relax(r, true, true);
    // Snap near misses into line: a slight jog in a link reads as a mistake, not a choice.
    auto snap = [&]() {
        for (int round = 0; round < 2; ++round)
            for (const auto &layer : layers)
                for (int i = 0; i < layer.size(); ++i) {
                    const int item = layer[i];
                    for (const auto *neighbors : {&preds[item], &succs[item]}) {
                        if (neighbors->size() != 1)
                            continue;
                        const qreal target = x[neighbors->first().item];
                        if (qAbs(target - x[item]) < nodeGap / 2 &&
                            (i == 0 || x[layer[i - 1]] + separation(layer[i - 1], item) <= target) &&
                            (i + 1 == layer.size() || target + separation(item, layer[i + 1]) <= x[layer[i + 1]])) {
                            x[item] = target;
                            break;
                        }
                    }
                }
    };
    snap();
    // Square up open subgraphs: line up each side at its outermost position, widen for the
    // title, and push whatever that crowds further along. Only ever moving things forward,
    // this settles unless two subgraphs swap order between ranks; then keep the ragged sides.
    if (!groups.isEmpty()) {
        // Borders have no links pulling them along, so first draw each one in against its members.
        for (const auto &layer : layers) {
            for (int i = layer.size() - 2; i >= 0; --i)
                if (items[layer[i]].border < 0)
                    x[layer[i]] = x[layer[i + 1]] - separation(layer[i], layer[i + 1]);
            for (int i = 1; i < layer.size(); ++i)
                if (items[layer[i]].border > 0)
                    x[layer[i]] = x[layer[i - 1]] + separation(layer[i - 1], layer[i]);
        }
        const QVector<qreal> ragged = x;
        bool settled = false;
        for (int round = 0; round < 32 && !settled; ++round) {
            settled = true;
            auto move = [&](int i, qreal to) {
                if (x[i] < to - 0.01) {
                    x[i] = to;
                    settled = false;
                }
            };
            for (int g : groups) {
                qreal first = -std::numeric_limits<qreal>::max(), last = first;
                for (int i : starts[g])
                    first = qMax(first, x[i]);
                for (int i : ends[g])
                    last = qMax(last, x[i]);
                if (!across)
                    last = qMax(last, first + d.clusters[g].titleWidth + 2 * clusterPad);
                for (int i : starts[g])
                    move(i, first);
                for (int i : ends[g])
                    move(i, last);
            }
            for (const auto &layer : layers)
                for (int i = 1; i < layer.size(); ++i)
                    move(layer[i], x[layer[i - 1]] + separation(layer[i - 1], layer[i]));
        }
        if (!settled)
            x = ragged;
        // With the sides fixed, let each rank settle between them again.
        pinBorders = true;
        for (int round = 0; round < 4; ++round) {
            for (int r = 1; r < ranks; ++r)
                relax(r, true, false);
            for (int r = ranks - 2; r >= 0; --r)
                relax(r, false, true);
        }
        for (int r = 0; r < ranks; ++r)
            relax(r, true, true);
        snap();
    }

    // Ranks, with room between them for the tops and bottoms of the open subgraphs starting or
    // ending there, nested ones stacked inside their parents.
    QVector<qreal> thickness(ranks, 0), y(ranks, 0), before(ranks, 0), after(ranks, 0);
    for (const auto &item : items)
        thickness[item.rank] = qMax(thickness[item.rank], item.h);
    auto rim = [&](auto &self, int g, bool top) -> qreal {
        qreal inner = 0;
        for (int c : groups)
            if (d.clusters[c].parent == g &&
                (top ? span[c].first == span[g].first : span[c].second == span[g].second))
                inner = qMax(inner, self(self, c, top));
        // The title sits on whichever end of the ranks is the top of the slide.
        const bool titled = top ? dir == Dir::TB : dir == Dir::BT;
        return clusterPad + (titled ? d.clusters[g].titleHeight : 0) + inner;
    };
    for (int g : groups) {
        before[span[g].first] = qMax(before[span[g].first], rim(rim, g, true));
        after[span[g].second] = qMax(after[span[g].second], rim(rim, g, false));
    }
    y[0] = before[0] + thickness[0] / 2;
    for (int r = 1; r < ranks; ++r)
        y[r] = y[r - 1] + thickness[r - 1] / 2 + after[r - 1] + gap + before[r] + thickness[r] / 2;
    qreal left = std::numeric_limits<qreal>::max(), width = 0;
    for (int i = 0; i < n; ++i)
        left = qMin(left, x[i] - items[i].w / 2);
    for (int i = 0; i < n; ++i)
        width = qMax(width, (x[i] -= left) + items[i].w / 2);
    const qreal height = y[ranks - 1] + thickness[ranks - 1] / 2 + after[ranks - 1];
    auto point = [&](qreal along, qreal down) {
        switch (dir) {
        case Dir::BT: return QPointF(along, height - down);
        case Dir::LR: return QPointF(down, along);
        case Dir::RL: return QPointF(height - down, along);
        default: return QPointF(along, down);
        }
    };
    auto local = [&](int i) { return point(x[i], y[items[i].rank]); };
    for (int i = 0; i < real; ++i)
        (items[i].node >= 0 ? d.nodes[items[i].node].local : d.clusters[items[i].cluster].local) = local(i);
    for (int g : groups) {
        qreal first = std::numeric_limits<qreal>::max(), last = -first;
        for (int i : starts[g])
            first = qMin(first, x[i]);
        for (int i : ends[g])
            last = qMax(last, x[i]);
        const int top = span[g].first, bottom = span[g].second;
        d.clusters[g].box = QRectF(point(first, y[top] - thickness[top] / 2 - rim(rim, g, true)),
                                   point(last, y[bottom] + thickness[bottom] / 2 + rim(rim, g, false)))
                                .normalized();
    }
    // A link only needs to bend where a rank holds something to pass: skip its points in ranks
    // of bare link points, so it sweeps through them instead of hooking at each one.
    QVector<bool> crowded(ranks, false);
    for (const auto &item : items)
        if (!dummy(&item - items.data()) || item.label || item.border)
            crowded[item.rank] = true;
    for (const auto &link : links) {
        auto &edge = d.edges[link.edge];
        edge.middle.clear();
        edge.labelAt = -1;
        for (int i : link.chain) {
            if (!crowded[items[i].rank] && !items[i].label)
                continue;
            if (items[i].label)
                edge.labelAt = edge.middle.size();
            edge.middle.append(local(i));
        }
        if (link.reversed) {
            std::reverse(edge.middle.begin(), edge.middle.end());
            if (edge.labelAt >= 0)
                edge.labelAt = edge.middle.size() - 1 - edge.labelAt;
        }
    }
    return across ? QSizeF(height, width) : QSizeF(width, height);
}

QPointF flow(Dir dir) {
    switch (dir) {
    case Dir::BT: return {0, -1};
    case Dir::LR: return {1, 0};
    case Dir::RL: return {-1, 0};
    default: return {0, 1};
    }
}
QPointF unit(QPointF v) {
    const qreal length = qSqrt(QPointF::dotProduct(v, v));
    return length > 0 ? v / length : QPointF(0, 1);
}
qreal markerLength(Head head, Line line) {
    return head == Head::Arrow ? (line == Line::Thick ? 20 : 15) : head == Head::None ? 0 : 12;
}

QPainterPath outline(Shape shape, const QRectF &r) {
    QPainterPath path;
    const qreal k = r.height() / 4, cx = r.center().x(), cy = r.center().y();
    auto polygon = [&](std::initializer_list<QPointF> points) {
        path.addPolygon(QPolygonF(QVector<QPointF>(points)));
        path.closeSubpath();
    };
    switch (shape) {
    case Shape::Round: path.addRoundedRect(r, 12, 12); break;
    case Shape::Stadium: path.addRoundedRect(r, r.height() / 2, r.height() / 2); break;
    case Shape::Circle:
    case Shape::DoubleCircle: path.addEllipse(r); break;
    case Shape::Cylinder: {
        const qreal cap = cylinderCap(r.width());
        path.moveTo(r.left(), r.top() + cap);
        path.arcTo(QRectF(r.left(), r.top(), r.width(), 2 * cap), 180, -180);
        path.lineTo(r.right(), r.bottom() - cap);
        path.arcTo(QRectF(r.left(), r.bottom() - 2 * cap, r.width(), 2 * cap), 0, -180);
        path.closeSubpath();
        break;
    }
    case Shape::Diamond: polygon({{cx, r.top()}, {r.right(), cy}, {cx, r.bottom()}, {r.left(), cy}}); break;
    case Shape::Hexagon:
        polygon({{r.left() + k, r.top()}, {r.right() - k, r.top()}, {r.right(), cy},
                 {r.right() - k, r.bottom()}, {r.left() + k, r.bottom()}, {r.left(), cy}});
        break;
    case Shape::Lean:
        polygon({{r.left() + k, r.top()}, r.topRight(), {r.right() - k, r.bottom()}, r.bottomLeft()});
        break;
    case Shape::LeanAlt:
        polygon({r.topLeft(), {r.right() - k, r.top()}, r.bottomRight(), {r.left() + k, r.bottom()}});
        break;
    case Shape::Trapezoid:
        polygon({{r.left() + k, r.top()}, {r.right() - k, r.top()}, r.bottomRight(), r.bottomLeft()});
        break;
    case Shape::TrapezoidAlt:
        polygon({r.topLeft(), r.topRight(), {r.right() - k, r.bottom()}, {r.left() + k, r.bottom()}});
        break;
    case Shape::Flag: polygon({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), {r.left() + k, cy}}); break;
    default: path.addRect(r);
    }
    return path;
}
// A smooth curve through points that leaves and arrives along the flow, so ends meet boxes square on.
QPainterPath spline(const QVector<QPointF> &points, QPointF f) {
    const int n = points.size();
    QVector<QPointF> tangent(n);
    tangent[0] = f * QPointF::dotProduct(points[1] - points[0], f) * 1.5;
    tangent[n - 1] = f * QPointF::dotProduct(points[n - 1] - points[n - 2], f) * 1.5;
    for (int i = 1; i + 1 < n; ++i)
        tangent[i] = (points[i + 1] - points[i - 1]) / 2;
    // A handle never reaches past half its own segment, so a short hop next to a long run
    // bends gently instead of overshooting into a hook.
    auto handle = [](QPointF tangent, qreal segment) {
        const qreal length = qSqrt(QPointF::dotProduct(tangent, tangent)) / 3;
        return length > segment / 2 ? tangent * (segment / 2 / (length * 3)) : tangent / 3;
    };
    QPainterPath path(points[0]);
    for (int i = 0; i + 1 < n; ++i) {
        const QPointF step = points[i + 1] - points[i];
        const qreal segment = qSqrt(QPointF::dotProduct(step, step));
        path.cubicTo(points[i] + handle(tangent[i], segment), points[i + 1] - handle(tangent[i + 1], segment),
                     points[i + 1]);
    }
    return path;
}

void layout(Diagram &d) {
    auto container = [&](End e) { return e.cluster ? d.clusters[e.index].parent : d.nodes[e.index].cluster; };
    auto ancestors = [&](End e) {
        QVector<int> chain;
        for (int c = container(e);; c = d.clusters[c].parent) {
            chain.append(c);
            if (c < 0)
                return chain;
        }
    };
    auto inside = [&](End e, int c) {
        for (int x = container(e); x >= 0; x = d.clusters[x].parent)
            if (x == c)
                return true;
        return false;
    };
    // A subgraph is open when a link reaches inside it from outside. Open subgraphs are laid
    // out with their surroundings so such links can route around their other members.
    for (auto &edge : d.edges) {
        if (edge.a.index < 0 || edge.b.index < 0 || (edge.a.cluster && inside(edge.b, edge.a.index)) ||
            (edge.b.cluster && inside(edge.a, edge.b.index)))
            continue;
        for (const auto &[from, to] : {std::make_pair(edge.a, edge.b), std::make_pair(edge.b, edge.a)})
            for (int c = container(from); c >= 0; c = d.clusters[c].parent)
                if (!inside(to, c))
                    d.clusters[c].open = true;
    }
    auto closed = [&](int c) {
        while (c >= 0 && d.clusters[c].open)
            c = d.clusters[c].parent;
        return c;
    };
    for (auto &node : d.nodes)
        node.host = closed(node.cluster);
    for (auto &cluster : d.clusters)
        cluster.host = closed(cluster.parent);
    // What stands for an end in a host's layout: the node, the closed subgraph holding it there,
    // or for a whole open subgraph, its first member.
    auto lift = [&](End e, int level) -> End {
        if (!e.cluster && d.nodes[e.index].host == level)
            return e;
        for (int c = e.cluster ? e.index : d.nodes[e.index].cluster; c >= 0; c = d.clusters[c].parent)
            if (!d.clusters[c].open && d.clusters[c].host == level)
                return End{true, c};
        for (int i = 0; i < d.nodes.size(); ++i)
            if (d.nodes[i].host == level && inside(End{false, i}, e.index))
                return End{false, i};
        for (int c = 0; c < d.clusters.size(); ++c)
            if (!d.clusters[c].open && d.clusters[c].host == level && inside(End{true, c}, e.index))
                return End{true, c};
        return e;
    };
    // Each link is laid out by the innermost closed subgraph holding both of its ends.
    for (auto &edge : d.edges) {
        if (edge.a.index < 0 || edge.b.index < 0 || (edge.a.cluster && inside(edge.b, edge.a.index)) ||
            (edge.b.cluster && inside(edge.a, edge.b.index)))
            continue;
        const auto outer = ancestors(edge.b);
        for (int c : ancestors(edge.a))
            if (outer.contains(c)) {
                edge.level = closed(c);
                break;
            }
        edge.liftedA = lift(edge.a, edge.level);
        edge.liftedB = lift(edge.b, edge.level);
        edge.loop = edge.a == edge.b && !edge.a.cluster;
        edge.drawable = edge.loop || !(edge.liftedA == edge.liftedB);
    }
    // Closed subgraphs lay out innermost first, so each knows its size when its parent lays out.
    int deepest = -1;
    for (const auto &cluster : d.clusters)
        deepest = qMax(deepest, cluster.depth);
    for (int depth = deepest; depth >= 0; --depth)
        for (int c = 0; c < d.clusters.size(); ++c) {
        auto &cluster = d.clusters[c];
        if (cluster.open || cluster.depth != depth)
            continue;
        const QSizeF content = layoutLevel(d, c);
        const qreal width = qMax(content.width(), cluster.titleWidth) + 2 * clusterPad;
        cluster.size = {qMax(width, 80.0), content.height() + 2 * clusterPad + cluster.titleHeight};
        cluster.content = {(cluster.size.width() - content.width()) / 2, clusterPad + cluster.titleHeight};
        }
    layoutLevel(d, -1);

    auto place = [&](auto &self, int level, QPointF origin) -> void {
        for (auto &node : d.nodes)
            if (node.host == level)
                node.rect = QRectF(origin + node.local - QPointF(node.size.width(), node.size.height()) / 2, node.size);
        for (int c = 0; c < d.clusters.size(); ++c) {
            auto &cluster = d.clusters[c];
            if (cluster.host != level)
                continue;
            if (cluster.open) {
                cluster.rect = cluster.box.translated(origin);
                continue;
            }
            cluster.rect = QRectF(origin + cluster.local - QPointF(cluster.size.width(), cluster.size.height()) / 2,
                                  cluster.size);
            self(self, c, cluster.rect.topLeft() + cluster.content);
        }
        for (auto &edge : d.edges)
            if (edge.level == level && edge.drawable)
                for (auto &point : edge.middle)
                    point += origin;
    };
    place(place, -1, {0, 0});

    auto rectOf = [&](End e) { return e.cluster ? d.clusters[e.index].rect : d.nodes[e.index].rect; };
    auto flowOf = [&](const Edge &edge) { return flow(edge.level < 0 ? d.dir : d.clusters[edge.level].dir); };
    // Links meet a box on the side facing their neighbor, spread across that side in the order
    // of their neighbors so several links neither pile onto one point nor cross at the box.
    struct Port {
        End end;
        int edge;
        bool source;
        QPointF toward, side;
        qreal along;
        bool sameSide(const Port &other) const { return end == other.end && side == other.side; }
    };
    QVector<Port> ports;
    for (int i = 0; i < d.edges.size(); ++i) {
        const auto &edge = d.edges[i];
        if (!edge.drawable || edge.loop)
            continue;
        const QPointF f = flowOf(edge);
        const QPointF first = edge.middle.isEmpty() ? rectOf(edge.b).center() : edge.middle.first();
        const QPointF last = edge.middle.isEmpty() ? rectOf(edge.a).center() : edge.middle.last();
        for (const auto &[end, toward, source] : {std::make_tuple(edge.a, first, true), std::make_tuple(edge.b, last, false)}) {
            const QPointF side = f * (QPointF::dotProduct(toward - rectOf(end).center(), f) < 0 ? -1 : 1);
            ports.append({end, i, source, toward, side, QPointF::dotProduct(toward, QPointF(qAbs(side.y()), qAbs(side.x())))});
        }
    }
    // Group ports by box and side, each group in order along that side.
    std::stable_sort(ports.begin(), ports.end(), [](const Port &a, const Port &b) {
        return std::tuple(a.end.cluster, a.end.index, a.side.x(), a.side.y(), a.along) <
               std::tuple(b.end.cluster, b.end.index, b.side.x(), b.side.y(), b.along);
    });
    QVector<QPointF> starts(d.edges.size()), ends(d.edges.size());
    for (int first = 0, count; first < ports.size(); first += count) {
        for (count = 1; first + count < ports.size() && ports[first + count].sameSide(ports[first]); ++count) {}
        const End end = ports[first].end;
        const QRectF r = rectOf(end);
        const Shape shape = end.cluster ? Shape::Rect : d.nodes[end.index].shape;
        const QPointF side = ports[first].side, across(qAbs(side.y()), qAbs(side.x()));
        const bool pointed = shape == Shape::Circle || shape == Shape::DoubleCircle ||
                             shape == Shape::Diamond || shape == Shape::Hexagon;
        const qreal usable = (side.x() != 0 ? r.height() : r.width()) * (pointed ? 0.35 : 0.6);
        const qreal spacing = count > 1 ? qMin(usable / (count - 1), 32.0) : 0;
        const QPainterPath body = outline(shape, r);
        for (int k = 0; k < count; ++k) {
            // Walk out from the center line to wherever the shape's outline is on this side.
            const QPointF from = r.center() + across * (k - (count - 1) / 2.0) * spacing;
            qreal inside = 0, outside = r.width() + r.height();
            for (int step = 0; step < 24; ++step) {
                const qreal middle = (inside + outside) / 2;
                (body.contains(from + side * middle) ? inside : outside) = middle;
            }
            (ports[first + k].source ? starts : ends)[ports[first + k].edge] = from + side * inside;
        }
    }
    d.bounds = QRectF();
    for (const auto &node : d.nodes)
        d.bounds |= node.rect;
    for (const auto &cluster : d.clusters)
        d.bounds |= cluster.rect;
    for (auto &edge : d.edges) {
        if (!edge.drawable)
            continue;
        if (edge.loop) {
            // Out across the flow and back, so the loop stays clear of links along it.
            const QRectF r = rectOf(edge.a);
            const QPointF f = flowOf(edge), out(f.y(), -f.x());
            const qreal spread = (out.x() != 0 ? r.height() : r.width()) / 4;
            const QPainterPath body = outline(d.nodes[edge.a.index].shape, r);
            auto boundary = [&](QPointF from) {
                qreal inside = 0, outside = r.width() + r.height();
                for (int step = 0; step < 24; ++step) {
                    const qreal middle = (inside + outside) / 2;
                    (body.contains(from + out * middle) ? inside : outside) = middle;
                }
                return from + out * inside;
            };
            const QPointF start = boundary(r.center() - f * spread), end = boundary(r.center() + f * spread);
            const qreal reach = loopReach + qMax(QPointF::dotProduct(start - r.center(), out),
                                                 QPointF::dotProduct(end - r.center(), out));
            const QPointF c1 = r.center() - f * spread + out * reach * 1.3, c2 = r.center() + f * spread + out * reach * 1.3;
            edge.headTip = end;
            edge.headDir = unit(end - c2);
            edge.path = QPainterPath(start);
            edge.path.cubicTo(c1, c2, end - edge.headDir * markerLength(edge.head, edge.line));
            const qreal labelExtent = out.x() != 0 ? edge.labelSize.width() : edge.labelSize.height();
            edge.labelPos = r.center() + out * (reach + labelExtent / 2 + 4);
        } else {
            const QPointF f = flowOf(edge);
            QVector<QPointF> points = edge.middle;
            points.prepend(starts[&edge - d.edges.data()]);
            points.append(ends[&edge - d.edges.data()]);
            auto tangent = [&](QPointF from, QPointF to) {
                const qreal along = QPointF::dotProduct(to - from, f);
                return qAbs(along) >= 1 ? f * (along > 0 ? 1 : -1) : unit(to - from);
            };
            const int n = points.size();
            if (edge.head != Head::None) {
                edge.headTip = points[n - 1];
                edge.headDir = tangent(points[n - 2], points[n - 1]);
                points[n - 1] -= edge.headDir * markerLength(edge.head, edge.line);
            }
            if (edge.tail != Head::None) {
                edge.tailTip = points[0];
                edge.tailDir = -tangent(points[0], points[1]);
                points[0] -= edge.tailDir * markerLength(edge.tail, edge.line);
            }
            edge.path = spline(points, f);
            edge.labelPos = edge.labelAt >= 0 ? edge.middle[edge.labelAt] : edge.path.pointAtPercent(0.5);
        }
        d.bounds |= edge.path.boundingRect();
        if (!edge.labelPos.isNull() && !edge.lines.isEmpty())
            d.bounds |= QRectF(edge.labelPos - QPointF(edge.labelSize.width(), edge.labelSize.height()) / 2,
                               edge.labelSize);
    }
}

std::shared_ptr<const Diagram> diagramFor(const QString &source, const QString &family) {
    static QMutex mutex;
    static QCache<QString, std::shared_ptr<const Diagram>> cache(128);
    const QString key = family + '\n' + source;
    {
        QMutexLocker lock(&mutex);
        if (const auto cached = cache.object(key))
            return *cached;
    }
    auto diagram = std::make_shared<Diagram>();
    Parser(*diagram).parse(source);
    if (diagram->error.isEmpty()) {
        measure(*diagram, family);
        layout(*diagram);
    }
    std::shared_ptr<const Diagram> result = diagram;
    QMutexLocker lock(&mutex);
    cache.insert(key, new std::shared_ptr<const Diagram>(result));
    return result;
}

QColor mix(const QColor &a, const QColor &b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}
QColor inkOn(const QColor &fill) {
    return fill.redF() * 0.2126 + fill.greenF() * 0.7152 + fill.blueF() * 0.0722 > .55 ? QColor("#161616")
                                                                                          : QColor("#ffffff");
}
void drawLines(QPainter *p, const QStringList &lines, QPointF center, qreal spacing) {
    const qreal top = center.y() - lines.size() * spacing / 2;
    for (int i = 0; i < lines.size(); ++i)
        p->drawText(QRectF(center.x() - 2000, top + i * spacing, 4000, spacing), Qt::AlignCenter, lines[i]);
}
void drawMarker(QPainter *p, Head head, QPointF tip, QPointF dir, Line line, const QColor &color) {
    const QPointF normal(-dir.y(), dir.x());
    const qreal length = markerLength(head, line);
    p->save();
    if (head == Head::Arrow) {
        const qreal half = line == Line::Thick ? 11 : 8;
        const QPointF base = tip - dir * length;
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawPolygon(QPolygonF({tip, base + normal * half, base - normal * half}));
    } else if (head == Head::Circle) {
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawEllipse(tip - dir * length / 2, length / 2, length / 2);
    } else if (head == Head::Cross) {
        const QPointF c = tip - dir * length / 2, a = (dir + normal) * length / 2.8, b = (dir - normal) * length / 2.8;
        p->setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(c - a, c + a);
        p->drawLine(c - b, c + b);
    }
    p->restore();
}
} // namespace

QString mermaidProblem(const QString &source) {
    Diagram diagram;
    Parser(diagram).parse(source);
    return diagram.error;
}

QHash<QString, QRectF> mermaidNodeRects(const QString &source, const QString &font) {
    QHash<QString, QRectF> rects;
    const auto diagram = diagramFor(source, font);
    for (const auto &node : diagram->nodes)
        rects.insert(node.id, node.rect);
    for (const auto &cluster : diagram->clusters)
        rects.insert("subgraph:" + cluster.id, cluster.rect);
    return rects;
}

QList<MermaidLink> mermaidLinks(const QString &source, const QString &font) {
    QList<MermaidLink> links;
    const auto diagram = diagramFor(source, font);
    for (const auto &edge : diagram->edges)
        if (edge.drawable && edge.line != Line::Invisible)
            links.append({edge.from, edge.to, edge.path});
    return links;
}

qreal paintMermaid(QPainter *p, const QRectF &area, const QString &source, const QVariantMap &palette) {
    const QString family = palette.value("font", "JetBrains Mono").toString();
    const auto diagram = diagramFor(source, family);
    const Diagram &d = *diagram;
    if (!d.error.isEmpty() || d.bounds.isEmpty())
        return 0;
    const qreal scale =
        qMin(maxScale, qMin(area.width() / d.bounds.width(), area.height() / d.bounds.height()));
    const QColor background(palette.value("background").toString()),
        foreground(palette.value("foreground").toString()), accent(palette.value("accent").toString());
    const QColor nodeFill = mix(background, accent, 0.14), lineColor = mix(background, foreground, 0.7);
    const QFont text = diagramFont(family, textSize), label = diagramFont(family, labelSize),
                title = diagramFont(family, titleSize, true);
    p->save();
    p->translate(area.center());
    p->scale(scale, scale);
    p->translate(-d.bounds.center());

    auto clusterFill = [&](int c) {
        const auto &cluster = d.clusters[c];
        return cluster.style.fill.isValid() ? cluster.style.fill : mix(background, foreground, 0.05 + 0.03 * cluster.depth);
    };
    QVector<int> clusters; // outermost first, so inner boxes paint over their parents
    for (int depth = 0; clusters.size() < d.clusters.size(); ++depth)
        for (int c = 0; c < d.clusters.size(); ++c)
            if (d.clusters[c].depth == depth)
                clusters.append(c);
    for (int c : clusters) {
        const auto &cluster = d.clusters[c];
        const QColor fill = clusterFill(c);
        p->setPen(QPen(cluster.style.stroke.isValid() ? cluster.style.stroke : mix(background, foreground, 0.28),
                       cluster.style.width > 0 ? cluster.style.width : 2));
        p->setBrush(fill);
        p->drawRoundedRect(cluster.rect, 12, 12);
    }
    for (const auto &edge : d.edges) {
        if (!edge.drawable || edge.line == Line::Invisible)
            continue;
        QPen pen(lineColor, edge.line == Line::Thick ? 5 : 2.5);
        if (edge.line == Line::Dotted)
            pen.setDashPattern({2, 2});
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(edge.path);
        drawMarker(p, edge.head, edge.headTip, edge.headDir, edge.line, lineColor);
        drawMarker(p, edge.tail, edge.tailTip, edge.tailDir, edge.line, lineColor);
    }
    // Titles sit top-left, clear of links entering the middle of a box. They come after the links,
    // on a cutout like a link label's, so a link passing a narrow box doesn't strike through one.
    const QFontMetricsF titleMetrics(title);
    p->setFont(title);
    for (int c : clusters) {
        const auto &cluster = d.clusters[c];
        const QColor fill = clusterFill(c);
        for (int i = 0; i < cluster.lines.size(); ++i) {
            const QRectF line(cluster.rect.left() + clusterPad, cluster.rect.top() + clusterPad * 0.6 +
                              i * titleMetrics.lineSpacing(), titleMetrics.horizontalAdvance(cluster.lines[i]),
                              titleMetrics.lineSpacing());
            p->setPen(Qt::NoPen);
            p->setBrush(fill);
            p->drawRect(line.adjusted(-6, 0, 6, 0));
            p->setPen(cluster.style.text.isValid() ? cluster.style.text
                      : cluster.style.fill.isValid() ? inkOn(fill)
                                                     : mix(background, foreground, 0.8));
            p->drawText(line.adjusted(0, 0, 100, 0), Qt::AlignLeft | Qt::AlignVCenter, cluster.lines[i]);
        }
    }
    const qreal labelSpacing = QFontMetricsF(label).lineSpacing();
    for (const auto &edge : d.edges) {
        if (!edge.drawable || edge.lines.isEmpty())
            continue;
        // A label box takes the color of whatever it sits on, so it reads as a gap in the line.
        QColor under = background;
        int depth = -1;
        for (int c = 0; c < d.clusters.size(); ++c)
            if (d.clusters[c].depth > depth && d.clusters[c].rect.contains(edge.labelPos)) {
                depth = d.clusters[c].depth;
                under = clusterFill(c);
            }
        p->setPen(Qt::NoPen);
        p->setBrush(under);
        p->drawRoundedRect(QRectF(edge.labelPos - QPointF(edge.labelSize.width(), edge.labelSize.height()) / 2,
                                  edge.labelSize), 6, 6);
        p->setPen(foreground);
        p->setFont(label);
        drawLines(p, edge.lines, edge.labelPos, labelSpacing);
    }
    const qreal textSpacing = QFontMetricsF(text).lineSpacing();
    for (const auto &node : d.nodes) {
        const QRectF r = node.rect;
        const QColor fill = node.style.fill.isValid() ? node.style.fill : nodeFill;
        QPen pen(node.style.stroke.isValid() ? node.style.stroke : accent, node.style.width > 0 ? node.style.width : 2.5);
        if (node.style.dashed)
            pen.setDashPattern({4, 3});
        p->setPen(pen);
        p->setBrush(fill);
        p->drawPath(outline(node.shape, r));
        p->setBrush(Qt::NoBrush);
        QPointF center = r.center();
        if (node.shape == Shape::Subroutine) {
            p->drawLine(QPointF(r.left() + 12, r.top()), QPointF(r.left() + 12, r.bottom()));
            p->drawLine(QPointF(r.right() - 12, r.top()), QPointF(r.right() - 12, r.bottom()));
        } else if (node.shape == Shape::Cylinder) {
            const qreal cap = cylinderCap(r.width());
            QPainterPath rim(QPointF(r.left(), r.top() + cap));
            rim.arcTo(QRectF(r.left(), r.top(), r.width(), 2 * cap), 180, 180);
            p->drawPath(rim);
            center.ry() += cap / 2;
        } else if (node.shape == Shape::DoubleCircle)
            p->drawEllipse(r.adjusted(7, 7, -7, -7));
        p->setPen(node.style.text.isValid() ? node.style.text : node.style.fill.isValid() ? inkOn(fill) : foreground);
        p->setFont(text);
        drawLines(p, node.lines, center, textSpacing);
    }
    p->restore();
    return textSize * scale;
}
