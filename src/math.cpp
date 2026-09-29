#include "math.h"
#include <algorithm>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHash>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QRegularExpression>
#include <QSet>
#include <QTextCursor>
#include <QTextDocument>
#include <QtMath>

// Delimiters follow Obsidian, which follows the same idea as Pandoc:
//   $...$  is inline only when the opening $ is not followed by whitespace,
//          the closing $ is not preceded by whitespace, and the closing $
//          is not followed by a digit. Inline math stays on one line.
//   $$...$$ is a displayed equation, may span lines, and may have space
//          just inside the dollars.
// A price such as "$1000 and $50" fails both the space test and the digit
// test, so it stays text. \$ is a literal dollar. Code and comments are not math.

namespace {

struct Range {
    int start, end;
};

bool escaped(const QString &text, int index) {
    int slashes = 0;
    for (int i = index - 1; i >= 0 && text[i] == '\\'; --i)
        ++slashes;
    return slashes % 2 == 1;
}

bool isSpace(QChar c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == QChar(0x00A0) ||
           c.category() == QChar::Separator_Space;
}

// Whole lines that are fenced or indented code. Mirrors outsideCode's block mask
// so a dollar in a code block is never a delimiter. The newline itself is left
// out of the range; a delimiter search that would enter the range fails instead.
QVector<Range> codeBlocks(const QString &source) {
    QVector<Range> ranges;
    int position = 0, fenceLength = 0;
    QChar fence;
    static const QRegularExpression marker("^ {0,3}(`{3,}|~{3,})(.*)$");
    while (position < source.size()) {
        int end = source.indexOf('\n', position);
        if (end < 0)
            end = source.size();
        const QString line = source.mid(position, end - position);
        const auto match = marker.match(line);
        bool protect = fenceLength > 0;
        if (match.hasMatch()) {
            const QString run = match.captured(1);
            protect = true;
            if (!fenceLength) {
                fence = run[0];
                fenceLength = run.size();
            } else if (run[0] == fence && run.size() >= fenceLength &&
                       match.captured(2).trimmed().isEmpty())
                fenceLength = 0;
        }
        if (protect || line.startsWith("    "))
            ranges.append({position, end});
        position = end + 1;
    }
    return ranges;
}

int skipCodeSpan(const QString &text, int index) {
    if (text[index] != '`')
        return -1;
    int ticks = 0;
    while (index + ticks < text.size() && text[index + ticks] == '`')
        ++ticks;
    int j = index + ticks;
    while (j < text.size()) {
        if (text[j] != '`') {
            ++j;
            continue;
        }
        int run = 0;
        while (j + run < text.size() && text[j + run] == '`')
            ++run;
        if (run == ticks)
            return j + run;
        j += run;
    }
    return -1;
}

int skipComment(const QString &text, int index) {
    if (!text.mid(index).startsWith("<!--"))
        return -1;
    const int end = text.indexOf("-->", index + 4);
    return end < 0 ? -1 : end + 3;
}

// First `$` at brace depth 0. Escapes and \{ \} do not change depth.
// Returns -1 if a code block is entered (the match fails) or the line ends
// for inline math.
int findCloser(const QString &text, int from, bool display, const QVector<Range> &blocks) {
    int depth = 0;
    int block = 0;
    for (int j = from; j < text.size(); ++j) {
        while (block < blocks.size() && blocks[block].end <= j)
            ++block;
        if (block < blocks.size() && j >= blocks[block].start)
            return -1;
        const QChar c = text[j];
        if (!display && (c == '\n' || c == '\r'))
            return -1;
        if (c == '\\' && j + 1 < text.size()) {
            ++j;
            continue;
        }
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            if (depth)
                --depth;
            continue;
        }
        if (c != '$' || depth)
            continue;
        if (display) {
            if (j + 1 < text.size() && text[j + 1] == '$')
                return j;
            continue;
        }
        return j;
    }
    return -1;
}

} // namespace

QVector<MathSpan> findMath(const QString &markdown) {
    const QVector<Range> blocks = codeBlocks(markdown);
    QVector<MathSpan> spans;
    int block = 0;
    for (int i = 0; i < markdown.size();) {
        while (block < blocks.size() && blocks[block].end <= i)
            ++block;
        if (block < blocks.size() && i >= blocks[block].start) {
            i = blocks[block].end;
            continue;
        }
        if (const int comment = skipComment(markdown, i); comment >= 0) {
            i = comment;
            continue;
        }
        if (markdown[i] == '`') {
            if (const int code = skipCodeSpan(markdown, i); code >= 0) {
                i = code;
                continue;
            }
        }
        if (markdown[i] != '$' || escaped(markdown, i)) {
            ++i;
            continue;
        }
        const bool display = i + 1 < markdown.size() && markdown[i + 1] == '$';
        if (!display && (i + 1 >= markdown.size() || isSpace(markdown[i + 1]))) {
            ++i;
            continue;
        }
        const int content = i + (display ? 2 : 1);
        const int closer = findCloser(markdown, content, display, blocks);
        if (closer < 0) {
            ++i;
            continue;
        }
        const QString tex = markdown.mid(content, closer - content);
        const bool digitAfter =
            !display && closer + 1 < markdown.size() && markdown[closer + 1].isDigit();
        const bool spaced = !display && (tex.isEmpty() || isSpace(tex.back()));
        if (digitAfter || spaced || tex.trimmed().isEmpty()) {
            ++i;
            continue;
        }
        MathSpan span;
        span.start = i;
        span.length = (display ? closer + 2 : closer + 1) - i;
        span.tex = tex.trimmed();
        span.display = display;
        spans.append(span);
        i = span.start + span.length;
    }
    return spans;
}

QString markdownWithMath(const QString &markdown, const QVector<MathSpan> &spans) {
    QString out = markdown;
    for (int i = spans.size() - 1; i >= 0; --i) {
        const MathSpan &span = spans[i];
        const QString token = QChar(0xE000) + QString("HYPEMATH%1").arg(i) + QChar(0xE001);
        if (!span.display) {
            out.replace(span.start, span.length, token);
            continue;
        }
        // A displayed equation is its own block, even when $$ sits mid-sentence.
        int from = span.start, to = span.start + span.length;
        while (from > 0 && (out[from - 1] == ' ' || out[from - 1] == '\t'))
            --from;
        while (to < out.size() && (out[to] == ' ' || out[to] == '\t'))
            ++to;
        const bool lineStart = from == 0 || out[from - 1] == '\n' || out[from - 1] == '\r';
        const bool lineEnd = to >= out.size() || out[to] == '\n' || out[to] == '\r';
        QString block = token;
        if (!lineStart)
            block.prepend("\n\n");
        if (!lineEnd)
            block.append("\n\n");
        out.replace(from, to - from, block);
    }
    return out;
}

namespace math_detail {

enum Style { StyDisplay, StyText, StyScript, StyScriptScript };
enum Class { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

struct Node {
    enum Type { Empty, Glyph, Row, Stack, Frac, Root, Accent, Space, Matrix, Fence } type = Empty;
    QString text;
    int cls = Ord;
    int style = StyText;
    bool big = false;
    bool bold = false;
    bool limits = false;
    bool nolimits = false;
    int alphabet = 1; // 0 math italic, 1 roman, 2 bold, 3 bold italic, 4 bb, 5 script, 6 sans, 7 mono, 8 italic text
    QChar left, right;
    QString align;
    int cols = 1;
    qreal w = 0, a = 0, d = 0, x = 0, y = 0;
    QVector<Node> kids;
};

QString ucs(char32_t code) {
    return QString::fromUcs4(&code, 1);
}

qreal styleScale(int style) {
    return style == StyScript ? 0.70 : style == StyScriptScript ? 0.50 : 1.0;
}

int scriptStyle(int style) {
    return style == StyDisplay || style == StyText ? StyScript : StyScriptScript;
}

bool mathAlphabetAvailable() {
    static const bool has = [] {
        QFont font("Noto Sans Math");
        font.setPixelSize(32);
        const QRawFont raw = QRawFont::fromFont(font);
        return raw.isValid() && raw.supportsCharacter(QChar(0x1D44E));
    }();
    return has;
}

QFont mathFont(qreal pixelSize, bool italic) {
    QFont font(mathAlphabetAvailable() ? "Noto Sans Math" : "Noto Sans");
    font.setPixelSize(qMax(1, qRound(pixelSize)));
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    if (italic && !mathAlphabetAvailable())
        font.setItalic(true);
    return font;
}

struct Symbol {
    char32_t code;
    int cls;
    bool big;
    bool sideLimits; // \int keeps scripts on the side unless \limits is asked for
};

const QHash<QString, Symbol> &symbols() {
    static const QHash<QString, Symbol> table = [] {
        QHash<QString, Symbol> t;
        auto put = [&](const char *name, char32_t code, int cls, bool big = false, bool side = false) {
            t.insert(QString::fromLatin1(name), Symbol{code, cls, big, side});
        };
        const struct { const char *n; char32_t c; } greek[] = {
            {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3}, {"delta", 0x03B4},
            {"epsilon", 0x03F5}, {"varepsilon", 0x03B5}, {"zeta", 0x03B6}, {"eta", 0x03B7},
            {"theta", 0x03B8}, {"vartheta", 0x03D1}, {"iota", 0x03B9}, {"kappa", 0x03BA},
            {"lambda", 0x03BB}, {"mu", 0x03BC}, {"nu", 0x03BD}, {"xi", 0x03BE},
            {"pi", 0x03C0}, {"varpi", 0x03D6}, {"rho", 0x03C1}, {"varrho", 0x03F1},
            {"sigma", 0x03C3}, {"varsigma", 0x03C2}, {"tau", 0x03C4}, {"upsilon", 0x03C5},
            {"phi", 0x03D5}, {"varphi", 0x03C6}, {"chi", 0x03C7}, {"psi", 0x03C8},
            {"omega", 0x03C9}, {"Gamma", 0x0393}, {"Delta", 0x0394}, {"Theta", 0x0398},
            {"Lambda", 0x039B}, {"Xi", 0x039E}, {"Pi", 0x03A0}, {"Sigma", 0x03A3},
            {"Upsilon", 0x03A5}, {"Phi", 0x03A6}, {"Psi", 0x03A8}, {"Omega", 0x03A9},
        };
        for (auto g : greek)
            put(g.n, g.c, Ord);
        const struct { const char *n; char32_t c; } bin[] = {
            {"pm", 0x00B1}, {"mp", 0x2213}, {"times", 0x00D7}, {"div", 0x00F7},
            {"cdot", 0x22C5}, {"circ", 0x2218}, {"bullet", 0x2219}, {"star", 0x22C6},
            {"ast", 0x2217}, {"oplus", 0x2295}, {"ominus", 0x2296}, {"otimes", 0x2297},
            {"oslash", 0x2298}, {"odot", 0x2299}, {"wedge", 0x2227}, {"vee", 0x2228},
            {"cap", 0x2229}, {"cup", 0x222A}, {"sqcap", 0x2293}, {"sqcup", 0x2294},
            {"uplus", 0x228E}, {"dagger", 0x2020}, {"ddagger", 0x2021},
            {"setminus", 0x2216}, {"triangleleft", 0x25C1}, {"triangleright", 0x25B7},
        };
        for (auto b : bin)
            put(b.n, b.c, Bin);
        const struct { const char *n; char32_t c; } rel[] = {
            {"leq", 0x2264}, {"le", 0x2264}, {"geq", 0x2265}, {"ge", 0x2265},
            {"neq", 0x2260}, {"ne", 0x2260}, {"ll", 0x226A}, {"gg", 0x226B},
            {"equiv", 0x2261}, {"sim", 0x223C}, {"simeq", 0x2243}, {"approx", 0x2248},
            {"cong", 0x2245}, {"propto", 0x221D}, {"prec", 0x227A}, {"succ", 0x227B},
            {"preceq", 0x2AAF}, {"succeq", 0x2AB0}, {"subset", 0x2282}, {"supset", 0x2283},
            {"subseteq", 0x2286}, {"supseteq", 0x2287}, {"in", 0x2208}, {"ni", 0x220B},
            {"notin", 0x2209}, {"mid", 0x2223}, {"parallel", 0x2225}, {"perp", 0x27C2},
            {"asymp", 0x224D}, {"doteq", 0x2250}, {"bowtie", 0x22C8}, {"models", 0x22A8},
            {"to", 0x2192}, {"rightarrow", 0x2192}, {"leftarrow", 0x2190}, {"gets", 0x2190},
            {"leftrightarrow", 0x2194}, {"Rightarrow", 0x21D2}, {"implies", 0x21D2},
            {"Leftarrow", 0x21D0}, {"Leftrightarrow", 0x21D4}, {"iff", 0x21D4},
            {"mapsto", 0x21A6}, {"longrightarrow", 0x27F6}, {"longleftarrow", 0x27F5},
            {"longmapsto", 0x27FC}, {"uparrow", 0x2191}, {"downarrow", 0x2193},
            {"nearrow", 0x2197}, {"searrow", 0x2198}, {"swarrow", 0x2199}, {"nwarrow", 0x2196},
        };
        for (auto r : rel)
            put(r.n, r.c, Rel);
        const struct { const char *n; char32_t c; } ord[] = {
            {"infty", 0x221E}, {"partial", 0x2202}, {"nabla", 0x2207}, {"forall", 0x2200},
            {"exists", 0x2203}, {"nexists", 0x2204}, {"neg", 0x00AC}, {"lnot", 0x00AC},
            {"emptyset", 0x2205}, {"varnothing", 0x2205}, {"angle", 0x2220},
            {"triangle", 0x25B3}, {"square", 0x25A1}, {"top", 0x22A4}, {"bot", 0x22A5},
            {"ell", 0x2113}, {"hbar", 0x210F}, {"imath", 0x0131}, {"jmath", 0x0237},
            {"wp", 0x2118}, {"Re", 0x211C}, {"Im", 0x2111}, {"aleph", 0x2135},
            {"prime", 0x2032}, {"ldots", 0x2026}, {"dots", 0x2026}, {"cdots", 0x22EF},
            {"vdots", 0x22EE}, {"ddots", 0x22F1}, {"therefore", 0x2234}, {"because", 0x2235},
            {"degree", 0x00B0}, {"backslash", 0x2216}, {"vert", '|'}, {"Vert", 0x2016},
            {"lbrace", '{'}, {"rbrace", '}'}, {"langle", 0x27E8}, {"rangle", 0x27E9},
            {"lfloor", 0x230A}, {"rfloor", 0x230B}, {"lceil", 0x2308}, {"rceil", 0x2309},
            {"colon", 0x2236},
        };
        for (auto o : ord)
            put(o.n, o.c, Ord);
        t["vert"].cls = Rel;
        t["Vert"].cls = Rel;
        t["lbrace"].cls = Open;
        t["rbrace"].cls = Close;
        t["langle"].cls = Open;
        t["rangle"].cls = Close;
        t["lfloor"].cls = Open;
        t["rfloor"].cls = Close;
        t["lceil"].cls = Open;
        t["rceil"].cls = Close;
        t["backslash"].cls = Ord;
        t["colon"].cls = Rel;
        const struct { const char *n; char32_t c; bool side; } big[] = {
            {"sum", 0x2211, false}, {"prod", 0x220F, false}, {"coprod", 0x2210, false},
            {"int", 0x222B, true}, {"iint", 0x222C, true}, {"iiint", 0x222D, true},
            {"oint", 0x222E, true}, {"bigcup", 0x22C3, false}, {"bigcap", 0x22C2, false},
            {"bigvee", 0x22C1, false}, {"bigwedge", 0x22C0, false}, {"bigoplus", 0x2A01, false},
            {"bigotimes", 0x2A02, false}, {"bigodot", 0x2A00, false}, {"bigsqcup", 0x2A06, false},
        };
        for (auto b : big)
            put(b.n, b.c, Op, true, b.side);
        return t;
    }();
    return table;
}

const QSet<QString> &functions() {
    static const QSet<QString> names = {
        "sin", "cos", "tan", "cot", "sec", "csc", "log", "ln", "exp", "lim", "sup", "inf",
        "max", "min", "det", "gcd", "dim", "ker", "hom", "arg", "deg", "arcsin", "arccos",
        "arctan", "sinh", "cosh", "tanh", "coth", "liminf", "limsup", "Pr"};
    return names;
}

bool limitsByDefault(const QString &name) {
    return name == "lim" || name == "sup" || name == "inf" || name == "max" || name == "min" ||
           name == "det" || name == "gcd" || name == "liminf" || name == "limsup" || name == "Pr";
}

QString mathLetter(QChar c, int alphabet) {
    // 0 italic, 1 roman, 2 bold, 3 bold-italic, 4 blackboard, 5 script, 6 sans, 7 mono
    const bool lower = c >= 'a' && c <= 'z';
    const bool upper = c >= 'A' && c <= 'Z';
    const bool digit = c >= '0' && c <= '9';
    if (!mathAlphabetAvailable() || alphabet == 1 || (!lower && !upper && !(digit && alphabet == 2)))
        return QString(c);
    auto from = [&](char32_t base, QChar ch) {
        return ucs(base + ch.unicode() - (lower ? 'a' : upper ? 'A' : '0'));
    };
    if (digit && alphabet == 2)
        return from(0x1D7CE, c);
    if (lower || upper) {
        const char32_t bases[][2] = {
            {0x1D434, 0x1D44E}, {0, 0}, {0x1D400, 0x1D41A}, {0x1D468, 0x1D482},
            {0x1D538, 0x1D552}, {0x1D4D0, 0x1D4EA}, {0x1D5A0, 0x1D5BA}, {0x1D670, 0x1D68A},
        };
        if (alphabet == 4 && upper) {
            switch (c.unicode()) {
            case 'C': return QString(QChar(0x2102));
            case 'H': return QString(QChar(0x210D));
            case 'N': return QString(QChar(0x2115));
            case 'P': return QString(QChar(0x2119));
            case 'Q': return QString(QChar(0x211A));
            case 'R': return QString(QChar(0x211D));
            case 'Z': return QString(QChar(0x2124));
            default: break;
            }
        }
        const char32_t base = bases[alphabet][lower ? 1 : 0];
        if (base)
            return from(base, c);
    }
    return QString(c);
}

struct Parser {
    QString s;
    int i = 0;
    int style = StyText;
    bool ok = true;
    int depth = 0;

    void skip() {
        while (i < s.size()) {
            if (s[i] == '%' ) {
                while (i < s.size() && s[i] != '\n')
                    ++i;
                continue;
            }
            if (isSpace(s[i])) {
                ++i;
                continue;
            }
            break;
        }
    }
    bool see(QChar c) {
        skip();
        return i < s.size() && s[i] == c;
    }
    bool eat(QChar c) {
        if (!see(c))
            return false;
        ++i;
        return true;
    }
    QString command() {
        // i is at '\\'
        ++i;
        if (i >= s.size())
            return {};
        if (!s[i].isLetter()) {
            const QString name(s[i]);
            ++i;
            if (name == "\\" && i < s.size() && s[i] == '*')
                ++i; // \\*
            return name;
        }
        const int start = i;
        while (i < s.size() && s[i].isLetter())
            ++i;
        if (i < s.size() && s[i] == '*')
            ++i;
        return s.mid(start, i - start);
    }
    bool atCommand(const QString &name) {
        if (i >= s.size() || s[i] != '\\')
            return false;
        const int save = i;
        const QString got = command();
        if (got == name)
            return true;
        i = save;
        return false;
    }

    Node glyph(const QString &text, int cls, int alphabet = 0) {
        Node n;
        n.type = Node::Glyph;
        n.text = text;
        n.cls = cls;
        n.style = style;
        n.alphabet = alphabet;
        return n;
    }
    Node space(qreal ems) {
        Node n;
        n.type = Node::Space;
        n.w = ems;
        n.cls = Ord;
        n.style = style;
        return n;
    }

    Node parseRows(bool stopBrace, bool stopRight);
    Node parseCell(bool stopBrace, bool stopRight, bool *rowBreak, bool *amp, bool *stopped);
    Node parseAtom();
    Node parseScripts(Node base);
    Node argument();
    Node group();
    QString readDelim();
};

Node Parser::argument() {
    skip();
    if (i >= s.size()) {
        ok = false;
        return {};
    }
    if (s[i] == '{')
        return group();
    const int savedStyle = style;
    Node atom = parseAtom();
    style = savedStyle;
    return atom;
}

Node Parser::group() {
    if (!eat('{')) {
        ok = false;
        return {};
    }
    if (++depth > 80) {
        ok = false;
        return {};
    }
    const int saved = style;
    Node inner = parseRows(true, false);
    style = saved;
    --depth;
    if (!eat('}'))
        ok = false;
    return inner;
}

QString Parser::readDelim() {
    skip();
    if (i >= s.size())
        return ".";
    if (s[i] == '\\') {
        const QString name = command();
        if (name == "lbrace" || name == "{") return "{";
        if (name == "rbrace" || name == "}") return "}";
        if (name == "langle") return "<";
        if (name == "rangle") return ">";
        if (name == "vert" || name == "|") return "|";
        if (name == "Vert" || name == "|") return "||";
        if (name == "lfloor") return "L";
        if (name == "rfloor") return "J";
        if (name == "lceil") return "l";
        if (name == "rceil") return "r";
        if (name == ".") return ".";
        if (name == "backslash") return "\\";
        return name.isEmpty() ? "." : name.left(1);
    }
    const QChar c = s[i++];
    if (c == '|' && i < s.size() && s[i] == '|') {
        ++i;
        return "||";
    }
    if (c == '.') return ".";
    if (c == '<') return "<";
    if (c == '>') return ">";
    if (c == '{') return "{";
    if (c == '}') return "}";
    if (c == '/') return "/";
    return QString(c);
}

Node Parser::parseScripts(Node base) {
    Node sup, sub;
    bool hasSup = false, hasSub = false;
    while (ok) {
        skip();
        if (i >= s.size())
            break;
        if (s[i] == '\'') {
            int count = 0;
            while (i < s.size() && s[i] == '\'') {
                ++count;
                ++i;
            }
            Node primes;
            primes.type = Node::Row;
            primes.style = scriptStyle(style);
            for (int n = 0; n < count; ++n) {
                const int saved = style;
                style = scriptStyle(style);
                primes.kids.append(glyph(QString(QChar(0x2032)), Ord, 1));
                style = saved;
            }
            if (hasSup) {
                Node both;
                both.type = Node::Row;
                both.kids = {sup, primes};
                sup = both;
            } else
                sup = primes;
            hasSup = true;
            continue;
        }
        if (s[i] == '^' && !hasSup) {
            ++i;
            const int saved = style;
            style = scriptStyle(style);
            sup = argument();
            style = saved;
            hasSup = true;
            continue;
        }
        if (s[i] == '_' && !hasSub) {
            ++i;
            const int saved = style;
            style = scriptStyle(style);
            sub = argument();
            style = saved;
            hasSub = true;
            continue;
        }
        if ((s[i] == '^' && hasSup) || (s[i] == '_' && hasSub)) {
            Node wrapped;
            wrapped.type = Node::Fence;
            wrapped.cls = Ord;
            wrapped.style = base.style;
            wrapped.kids = {base};
            if (hasSup)
                wrapped.kids.append(sup);
            else
                wrapped.kids.append(Node());
            if (hasSub)
                wrapped.kids.append(sub);
            else
                wrapped.kids.append(Node());
            wrapped.limits = false;
            base = wrapped;
            hasSup = hasSub = false;
            continue;
        }
        break;
    }
    if (!hasSup && !hasSub)
        return base;
    Node n;
    n.type = Node::Fence;
    n.cls = Ord;
    n.style = base.style;
    n.limits = base.limits && !base.nolimits;
    n.kids = {base, hasSup ? sup : Node(), hasSub ? sub : Node()};
    return n;
}

Node Parser::parseAtom() {
    skip();
    if (i >= s.size())
        return {};
    if (s[i] == '{') {
        Node inner = group();
        inner.cls = Ord;
        return parseScripts(inner);
    }
    if (s[i] == '\\') {
        const QString name = command();
        if (name.isEmpty()) {
            ok = false;
            return {};
        }
        auto spaced = [&](qreal em) {
            Node n = space(em);
            return n;
        };
        if (name == ",") return spaced(3.0 / 18);
        if (name == ":") return spaced(4.0 / 18);
        if (name == ";") return spaced(5.0 / 18);
        if (name == "!") return spaced(-3.0 / 18);
        if (name == " ") return spaced(1.0 / 3);
        if (name == "quad") return spaced(1);
        if (name == "qquad") return spaced(2);
        if (name == "displaystyle") { style = StyDisplay; return Node(); }
        if (name == "textstyle") { style = StyText; return Node(); }
        if (name == "scriptstyle") { style = StyScript; return Node(); }
        if (name == "scriptscriptstyle") { style = StyScriptScript; return Node(); }
        if (name == "limits" || name == "nolimits")
            return {};
        if (name == "not") {
            Node inner = parseAtom();
            Node n;
            n.type = Node::Accent;
            n.text = "not";
            n.cls = Rel;
            n.style = style;
            n.kids = {inner};
            return n;
        }
        if (name == "left") {
            const QString left = readDelim();
            Node inner = parseRows(false, true);
            if (!atCommand("right"))
                ok = false;
            else
                command();
            const QString right = readDelim();
            Node n;
            n.type = Node::Fence;
            n.text = "delim";
            n.cls = Inner;
            n.style = style;
            n.left = left.isEmpty() ? QChar('.') : left[0];
            if (left == "||") n.left = QChar(0x2016);
            if (left == "<") n.left = QChar(0x27E8);
            if (left == ">") n.left = QChar(0x27E9);
            if (left == "{") n.left = '{';
            if (left == "}") n.left = '}';
            n.right = right == "||" ? QChar(0x2016) : right == "<" ? QChar(0x27E8)
                                                      : right == ">" ? QChar(0x27E9)
                                                      : right == "{" ? '{'
                                                      : right == "}" ? '}'
                                                      : right.isEmpty() ? QChar('.')
                                                                        : right[0];
            if (right == "||") n.right = QChar(0x2016);
            n.kids = {inner};
            return parseScripts(n);
        }
        if (name == "begin") {
            if (!eat('{')) { ok = false; return {}; }
            const int ns = i;
            while (i < s.size() && s[i] != '}')
                ++i;
            QString env = s.mid(ns, i - ns).trimmed();
            if (i < s.size())
                ++i;
            QString spec;
            if (env == "array" && see('{')) {
                const int save = i;
                ++i;
                const int ss = i;
                while (i < s.size() && s[i] != '}')
                    ++i;
                spec = s.mid(ss, i - ss);
                if (i < s.size())
                    ++i;
                if (spec.isEmpty() || !QRegularExpression("^[lcr]+$").match(spec).hasMatch()) {
                    i = save;
                    spec.clear();
                }
            }
            const int body = i;
            int depth = 0;
            while (i < s.size()) {
                if (s[i] == '\\') {
                    const int save = i;
                    const QString cmd = command();
                    if (cmd == "end" && depth == 0) {
                        if (!eat('{')) { ok = false; return {}; }
                        const int es = i;
                        while (i < s.size() && s[i] != '}')
                            ++i;
                        const QString end = s.mid(es, i - es).trimmed();
                        if (i < s.size())
                            ++i;
                        if (end != env)
                            continue;
                        Parser inner;
                        inner.s = s.mid(body, save - body);
                        inner.style = style;
                        Node grid = inner.parseRows(false, false);
                        if (!inner.ok)
                            ok = false;
                        if (grid.type == Node::Stack) {
                            Node rows;
                            rows.type = Node::Matrix;
                            rows.cols = 1;
                            rows.kids = grid.kids;
                            grid = rows;
                        } else if (grid.type != Node::Matrix) {
                            Node cell;
                            cell.type = Node::Matrix;
                            cell.cols = 1;
                            cell.kids = {grid};
                            grid = cell;
                        }
                        grid.cls = Inner;
                        grid.style = style;
                        if (!spec.isEmpty())
                            grid.align = spec;
                        grid.text = env;
                        if (env == "pmatrix") { grid.left = '('; grid.right = ')'; }
                        else if (env == "bmatrix") { grid.left = '['; grid.right = ']'; }
                        else if (env == "Bmatrix") { grid.left = '{'; grid.right = '}'; }
                        else if (env == "vmatrix") { grid.left = '|'; grid.right = '|'; }
                        else if (env == "Vmatrix") { grid.left = QChar(0x2016); grid.right = QChar(0x2016); }
                        else if (env == "cases") { grid.left = '{'; grid.right = '.'; grid.align = "ll"; }
                        return parseScripts(grid);
                    }
                    continue;
                }
                if (s[i] == '{')
                    ++depth;
                else if (s[i] == '}' && depth)
                    --depth;
                ++i;
            }
            ok = false;
            return {};
        }
        if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "cfrac") {
            const int saved = style;
            if (name == "dfrac" || name == "cfrac")
                style = StyDisplay;
            else if (name == "tfrac")
                style = StyText;
            const int numStyle = style == StyDisplay ? StyText : scriptStyle(style);
            const int denStyle = numStyle;
            style = numStyle;
            Node num = argument();
            style = denStyle;
            Node den = argument();
            style = saved;
            Node n;
            n.type = Node::Frac;
            n.cls = Inner;
            n.style = name == "tfrac" ? StyText : name == "dfrac" ? StyDisplay : saved;
            n.kids = {num, den};
            return parseScripts(n);
        }
        if (name == "binom" || name == "dbinom" || name == "tbinom") {
            const int saved = style;
            style = name == "dbinom" ? StyDisplay : name == "tbinom" ? StyText : style;
            const int child = style == StyDisplay ? StyText : scriptStyle(style);
            style = child;
            Node top = argument();
            Node bottom = argument();
            style = saved;
            Node n;
            n.type = Node::Frac;
            n.text = "binom";
            n.cls = Inner;
            n.style = name == "tbinom" ? StyText : name == "dbinom" ? StyDisplay : saved;
            n.left = '(';
            n.right = ')';
            n.kids = {top, bottom};
            return parseScripts(n);
        }
        if (name == "sqrt") {
            Node index;
            bool hasIndex = false;
            skip();
            if (i < s.size() && s[i] == '[') {
                ++i;
                const int saved = style;
                style = scriptStyle(style);
                const int start = i;
                int depth = 0;
                while (i < s.size() && !(s[i] == ']' && !depth)) {
                    if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
                    if (s[i] == '{') ++depth;
                    else if (s[i] == '}' && depth) --depth;
                    ++i;
                }
                Parser p;
                p.s = s.mid(start, i - start);
                p.style = style;
                index = p.parseRows(false, false);
                if (!p.ok) ok = false;
                if (i < s.size() && s[i] == ']') ++i;
                style = saved;
                hasIndex = true;
            }
            Node body = argument();
            Node n;
            n.type = Node::Root;
            n.cls = Ord;
            n.style = style;
            n.kids = {body};
            if (hasIndex)
                n.kids.append(index);
            return parseScripts(n);
        }
        if (name == "text" || name == "mbox" || name == "textrm" || name == "textbf" ||
            name == "textit" || name == "operatorname") {
            if (!eat('{')) { ok = false; return {}; }
            const bool bold = name == "textbf";
            const bool italic = name == "textit";
            Node row;
            row.type = Node::Row;
            row.cls = name == "operatorname" ? Op : Ord;
            row.style = style;
            while (i < s.size() && s[i] != '}') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    row.kids.append(glyph(QString(s[i++]), Ord, 1));
                    continue;
                }
                Node g = glyph(QString(s[i++]), Ord, italic ? 8 : 1);
                g.bold = bold;
                row.kids.append(g);
            }
            if (!eat('}'))
                ok = false;
            if (name == "operatorname")
                row.limits = false;
            return parseScripts(row);
        }
        if (name == "mathrm" || name == "mathbf" || name == "mathit" || name == "mathbb" ||
            name == "mathcal" || name == "mathsf" || name == "mathtt" || name == "boldsymbol" ||
            name == "bm") {
            const int alphabet = name == "mathbf" ? 2 : name == "mathit" || name == "boldsymbol" || name == "bm"
                                                       ? 3
                                   : name == "mathbb" ? 4
                                   : name == "mathcal" ? 5
                                   : name == "mathsf" ? 6
                                   : name == "mathtt" ? 7
                                                      : 1;
            if (!see('{')) {
                Node g = parseAtom();
                // A single following atom stays as parsed; the alphabet applies to a group.
                if (g.type == Node::Glyph && g.text.size() == 1 && g.text[0].isLetter())
                    g.text = mathLetter(g.text[0], alphabet);
                return g;
            }
            if (!eat('{')) { ok = false; return {}; }
            Node row;
            row.type = Node::Row;
            row.cls = Ord;
            row.style = style;
            const int saved = i;
            Q_UNUSED(saved);
            while (i < s.size() && s[i] != '}') {
                if (isSpace(s[i])) { ++i; continue; }
                if (s[i] == '\\') {
                    Node atom = parseAtom();
                    row.kids.append(atom);
                    continue;
                }
                if (s[i] == '{') {
                    row.kids.append(group());
                    continue;
                }
                if (s[i].isLetter() || s[i].isDigit()) {
                    Node g = glyph(mathLetter(s[i], alphabet), Ord, alphabet);
                    g.style = style;
                    ++i;
                    row.kids.append(g);
                    continue;
                }
                Node g = glyph(QString(s[i]), Ord, 1);
                ++i;
                row.kids.append(parseScripts(g));
            }
            if (!eat('}'))
                ok = false;
            return parseScripts(row);
        }
        if (name == "overline" || name == "underline" || name == "hat" || name == "bar" ||
            name == "vec" || name == "dot" || name == "ddot" || name == "tilde" ||
            name == "widehat" || name == "widetilde" || name == "overrightarrow" ||
            name == "overleftarrow" || name == "boxed") {
            Node body = argument();
            Node n;
            n.type = name == "boxed" ? Node::Fence : Node::Accent;
            n.text = name;
            n.cls = Ord;
            n.style = style;
            n.kids = {body};
            if (name == "boxed") {
                n.text = "box";
                n.cls = Inner;
            }
            return parseScripts(n);
        }
        if (name == "label" || name == "nonumber" || name == "notag") {
            if (name == "label" || see('{'))
                argument();
            return {};
        }
        if (name == "tag") {
            Node tag = argument();
            Node open = glyph("(", Ord, 1);
            Node close = glyph(")", Ord, 1);
            Node row;
            row.type = Node::Row;
            row.cls = Ord;
            row.style = style;
            row.kids = {space(1), open, tag, close};
            return row;
        }
        if (name == "pmod") {
            Node arg = argument();
            Node row;
            row.type = Node::Row;
            row.cls = Inner;
            row.style = style;
            row.kids = {space(1.0 / 3), glyph("(", Open, 1), glyph("mod", Ord, 1), space(1.0 / 3), arg,
                        glyph(")", Close, 1)};
            return row;
        }
        if (name == "%" || name == "$" || name == "&" || name == "#" || name == "_" ||
            name == "{" || name == "}") {
            const int cls = name == "{" ? Open : name == "}" ? Close : Ord;
            return parseScripts(glyph(name == "{" ? "{" : name == "}" ? "}" : name, cls, 1));
        }
        if (functions().contains(name)) {
            Node n = glyph(name, Op, 1);
            n.limits = limitsByDefault(name) && style == StyDisplay;
            n.cls = Op;
            skip();
            if (atCommand("limits")) {
                command();
                n.limits = true;
                n.nolimits = false;
            } else if (atCommand("nolimits")) {
                command();
                n.limits = false;
                n.nolimits = true;
            }
            return parseScripts(n);
        }
        if (const auto it = symbols().constFind(name); it != symbols().cend()) {
            Node n = glyph(ucs(it->code), it->cls, 1);
            n.big = it->big;
            n.limits = it->big && !it->sideLimits && style == StyDisplay;
            n.nolimits = it->sideLimits;
            skip();
            if (atCommand("limits")) {
                command();
                n.limits = true;
                n.nolimits = false;
            } else if (atCommand("nolimits")) {
                command();
                n.limits = false;
                n.nolimits = true;
            }
            return parseScripts(n);
        }
        // Unknown command: show the name so the author can see what was not rendered.
        return parseScripts(glyph("\\" + name, Ord, 1));
    }

    const QChar c = s[i++];
    if (c == '&' || c == '}') {
        --i;
        return {};
    }
    int cls = Ord;
    QString text(c);
    int alphabet = 0;
    if (c == '+' || c == '*' || c == QChar(0x00B1))
        cls = Bin;
    else if (c == '-' || c == QChar(0x2212)) {
        cls = Bin;
        text = QString(QChar(0x2212));
        alphabet = 1;
    } else if (c == '=' || c == '<' || c == '>' || c == ':' || c == QChar(0x2264) || c == QChar(0x2265))
        cls = Rel;
    else if (c == '(' || c == '[' || c == '{')
        cls = Open;
    else if (c == ')' || c == ']' || c == '}')
        cls = Close;
    else if (c == ',' || c == ';')
        cls = Punct;
    else if (c.isLetter())
        text = mathLetter(c, 0);
    else
        alphabet = 1;
    if (c == '\'') {
        text = QString(QChar(0x2032));
        alphabet = 1;
    }
    if (c == '^' || c == '_') {
        --i;
        return parseScripts(Node());
    }
    return parseScripts(glyph(text, cls, alphabet));
}

Node makeRow(const QVector<Node> &atoms, int style) {
    Node row;
    row.type = Node::Row;
    row.cls = Ord;
    row.style = style;
    for (const Node &atom : atoms)
        if (atom.type != Node::Empty)
            row.kids.append(atom);
    if (row.kids.isEmpty())
        return {};
    if (row.kids.size() == 1)
        return row.kids[0];
    return row;
}

Node Parser::parseCell(bool stopBrace, bool stopRight, bool *rowBreak, bool *amp, bool *stopped) {
    QVector<Node> atoms;
    auto finish = [&] {
        // \over / \atop / \choose split the cell they appear in.
        return makeRow(atoms, style);
    };
    while (ok && i <= s.size()) {
        skip();
        if (i >= s.size())
            break;
        if (stopBrace && s[i] == '}')
            break;
        if (s[i] == '&') {
            *amp = true;
            ++i;
            break;
        }
        if (s[i] == '\\') {
            const int save = i;
            const QString name = command();
            if (name == "\\" || name == "cr") {
                *rowBreak = true;
                break;
            }
            if (name == "over" || name == "atop" || name == "choose") {
                Node num = finish();
                Node den = parseCell(stopBrace, stopRight, rowBreak, amp, stopped);
                Node n;
                n.type = Node::Frac;
                n.text = name == "choose" ? "binom" : name == "atop" ? "atop" : "";
                n.cls = Inner;
                n.style = style;
                if (name == "choose") { n.left = '('; n.right = ')'; }
                n.kids = {num, den};
                *stopped = true;
                return n;
            }
            if (stopRight && name == "right") {
                i = save;
                *stopped = true;
                break;
            }
            if (name == "end") {
                i = save;
                *stopped = true;
                break;
            }
            i = save;
        }
        Node atom = parseAtom();
        if (atom.type != Node::Empty)
            atoms.append(atom);
        else if (i < s.size() && (s[i] == '}' || s[i] == '&'))
            break;
        else if (i >= s.size())
            break;
        else if (atom.type == Node::Empty && s[i] != '\\') {
            // parseAtom stopped without consuming, avoid a spin on a stopper.
            if (s[i] == '}' || s[i] == '&')
                break;
        }
    }
    return finish();
}

Node Parser::parseRows(bool stopBrace, bool stopRight) {
    QVector<QVector<Node>> rows;
    bool anyAmp = false;
    while (ok) {
        QVector<Node> cells;
        bool rowBreak = false;
        while (ok) {
            bool amp = false, stopped = false, br = false;
            Node cell = parseCell(stopBrace, stopRight, &br, &amp, &stopped);
            cells.append(cell);
            if (amp) {
                anyAmp = true;
                continue;
            }
            rowBreak = br;
            break;
        }
        rows.append(cells);
        if (rowBreak)
            continue;
        break;
    }
    if (rows.isEmpty())
        return {};
    const int cols = std::max_element(rows.begin(), rows.end(),
                                       [](const auto &a, const auto &b) { return a.size() < b.size(); })
                          ->size();
    if (rows.size() == 1 && cols <= 1 && !anyAmp)
        return rows[0].isEmpty() ? Node() : rows[0][0];
    Node grid;
    grid.type = rows.size() > 1 && cols <= 1 && !anyAmp ? Node::Stack : Node::Matrix;
    grid.cls = Inner;
    grid.style = style;
    grid.cols = cols;
    if (grid.type == Node::Stack) {
        for (const auto &row : rows)
            grid.kids.append(row.isEmpty() ? Node() : row[0]);
        return grid;
    }
    grid.type = Node::Matrix;
    for (const auto &row : rows) {
        for (int c = 0; c < cols; ++c)
            grid.kids.append(c < row.size() ? row[c] : Node());
    }
    if (anyAmp && grid.align.isEmpty()) {
        // align-like: right, left, right, left...
        for (int c = 0; c < cols; ++c)
            grid.align += (c % 2 == 0) ? 'r' : 'l';
    }
    return grid;
}

// The alphabet for a glyph was stashed in x during parsing (1 = roman symbol).
void layout(Node &n, qreal base);

qreal emOf(const Node &n, qreal base) {
    return base * styleScale(n.style);
}

void layout(Node &n, qreal base) {
    const qreal em = emOf(n, base);
    switch (n.type) {
    case Node::Empty:
        break;
    case Node::Space:
        n.w = n.w * em;
        n.a = n.d = 0;
        break;
    case Node::Glyph: {
        const bool fakeItalic = n.alphabet == 8 || (n.alphabet == 0 && !mathAlphabetAvailable());
        QFont font = mathFont(n.big ? em * (n.style == StyDisplay ? 1.6 : 1.15) : em, fakeItalic);
        if (n.bold)
            font.setBold(true);
        const QFontMetricsF fm(font);
        n.w = fm.horizontalAdvance(n.text);
        n.a = fm.ascent() * 0.80;
        n.d = fm.descent() * 0.25;
        if (n.text == QString(QChar(0x2032))) {
            n.a = em * 0.55;
            n.d = 0;
            n.w = em * 0.35;
        }
        break;
    }
    case Node::Row: {
        for (Node &k : n.kids)
            layout(k, base);
        // A binary operator with nothing to bind on one side becomes ordinary,
        // so a leading minus doesn't pick up a gap.
        for (int i = 0; i < n.kids.size(); ++i) {
            if (n.kids[i].cls != Bin)
                continue;
            const int prev = i ? n.kids[i - 1].cls : -1;
            const int next = i + 1 < n.kids.size() ? n.kids[i + 1].cls : -1;
            const bool leftOk = prev == Ord || prev == Close || prev == Inner;
            const bool rightOk = next == Ord || next == Open || next == Inner;
            if (!leftOk || !rightOk)
                n.kids[i].cls = Ord;
        }
        const bool script = n.style == StyScript || n.style == StyScriptScript;
        qreal x = 0;
        n.a = n.d = 0;
        for (int i = 0; i < n.kids.size(); ++i) {
            if (i) {
                static const int gap[8][8] = {
                    {0, 1, 2, 3, 0, 0, 0, 1}, {1, 1, 0, 3, 0, 0, 0, 1},
                    {2, 2, 0, 0, 2, 0, 0, 2}, {3, 3, 0, 0, 3, 0, 0, 3},
                    {0, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 0, 0, 0, 1},
                    {1, 1, 0, 1, 1, 1, 1, 1}, {1, 1, 2, 3, 1, 0, 1, 1},
                };
                int g = gap[qBound(0, n.kids[i - 1].cls, 7)][qBound(0, n.kids[i].cls, 7)];
                if (script && g > 1)
                    g = 0;
                x += (g == 0 ? 0 : g + 2) * em / 18.0;
            }
            n.kids[i].x = x;
            n.kids[i].y = 0;
            x += n.kids[i].w;
            n.a = qMax(n.a, n.kids[i].a);
            n.d = qMax(n.d, n.kids[i].d);
        }
        n.w = x;
        break;
    }
    case Node::Stack: {
        qreal width = 0, y = 0;
        const qreal gap = em * 0.35;
        for (int i = n.kids.size() - 1; i >= 0; --i) {
            layout(n.kids[i], base);
            width = qMax(width, n.kids[i].w);
        }
        for (int i = n.kids.size() - 1; i >= 0; --i) {
            n.kids[i].x = (width - n.kids[i].w) / 2;
            if (i == int(n.kids.size()) - 1)
                y = n.kids[i].d;
            else
                y += gap + n.kids[i].d;
            n.kids[i].y = y;
            y += n.kids[i].a;
        }
        n.w = width;
        n.a = y;
        n.d = 0;
        // Center the stack on the math axis instead of sitting on the baseline.
        const qreal shift = y / 2 - em * 0.2;
        for (Node &k : n.kids)
            k.y -= shift;
        n.a = shift + (y - shift);
        n.d = shift;
        // Recompute from shifted baselines.
        n.a = n.d = 0;
        for (const Node &k : n.kids) {
            n.a = qMax(n.a, k.y + k.a);
            n.d = qMax(n.d, -k.y + k.d);
        }
        break;
    }
    case Node::Frac: {
        for (Node &k : n.kids)
            layout(k, base);
        const Node &num = n.kids.value(0);
        const Node &den = n.kids.value(1);
        const qreal rule = n.text == "atop" || n.text == "binom" ? 0 : qMax(em * 0.045, 0.6);
        const qreal gap = em * (n.style == StyDisplay ? 0.22 : 0.12);
        const qreal axis = em * 0.22;
        n.w = qMax(num.w, den.w) + em * 0.2;
        if (n.kids.size() > 0)
            n.kids[0].x = (n.w - num.w) / 2;
        if (n.kids.size() > 1)
            n.kids[1].x = (n.w - den.w) / 2;
        if (n.kids.size() > 0)
            n.kids[0].y = axis + rule / 2 + gap + num.d;
        if (n.kids.size() > 1)
            n.kids[1].y = axis - (rule / 2 + gap + den.a);
        n.a = n.kids.isEmpty() ? em : n.kids[0].y + num.a;
        n.d = n.kids.size() < 2 ? em * 0.2 : -n.kids[1].y + den.d;
        break;
    }
    case Node::Root: {
        for (Node &k : n.kids)
            layout(k, base);
        const Node &body = n.kids.value(0);
        const qreal rule = qMax(em * 0.045, 0.6);
        const qreal surd = em * 0.55;
        const qreal gap = em * 0.08;
        if (n.kids.size() > 1) {
            n.kids[1].x = 0;
            n.kids[1].y = body.a + gap + em * 0.15;
        }
        const qreal indexW = n.kids.size() > 1 ? n.kids[1].w * 0.6 : 0;
        if (!n.kids.isEmpty()) {
            n.kids[0].x = indexW + surd + gap;
            n.kids[0].y = 0;
        }
        n.w = indexW + surd + gap + body.w + em * 0.08;
        n.a = body.a + gap + rule + em * 0.06;
        n.d = body.d;
        break;
    }
    case Node::Accent: {
        for (Node &k : n.kids)
            layout(k, base);
        const Node &body = n.kids.value(0);
        n.w = qMax(body.w, em * 0.4);
        if (!n.kids.isEmpty()) {
            n.kids[0].x = (n.w - body.w) / 2;
            n.kids[0].y = 0;
        }
        n.a = body.a + em * (n.text == "not" ? 0 : 0.35);
        n.d = body.d;
        if (n.text == "underline")
            n.d += em * 0.18;
        break;
    }
    case Node::Matrix: {
        if (n.kids.size() == 1 && n.kids[0].type == Node::Matrix && n.text.isEmpty()) {
            // A grid produced by parseRows was wrapped; unwrap if this node is the grid itself.
        }
        const int cols = qMax(1, n.cols);
        const int count = n.kids.size();
        const int rows = qMax(1, (count + cols - 1) / cols);
        QVector<qreal> colW(cols, 0), rowA(rows, 0), rowD(rows, 0);
        for (int i = 0; i < count; ++i) {
            layout(n.kids[i], base);
            const int r = i / cols, c = i % cols;
            colW[c] = qMax(colW[c], n.kids[i].w);
            rowA[r] = qMax(rowA[r], n.kids[i].a);
            rowD[r] = qMax(rowD[r], n.kids[i].d);
        }
        const qreal colGap = em * (n.align.contains('r') ? 0.4 : 0.7);
        const qreal pairGap = em * 0.9;
        const qreal rowGap = em * 0.35;
        QVector<qreal> colX(cols, 0);
        qreal width = 0;
        for (int c = 0; c < cols; ++c) {
            if (c) {
                const bool pair = n.align.size() >= cols && n.align[c - 1] == 'l' && n.align[c] == 'r';
                width += pair ? pairGap : colGap;
            }
            colX[c] = width;
            width += colW[c];
        }
        qreal y = 0;
        QVector<qreal> rowY(rows, 0);
        for (int r = rows - 1; r >= 0; --r) {
            if (r == rows - 1)
                y = rowD[r];
            else
                y += rowGap + rowD[r];
            rowY[r] = y;
            y += rowA[r];
        }
        for (int i = 0; i < count; ++i) {
            const int r = i / cols, c = i % cols;
            const QChar how = c < n.align.size() ? n.align[c] : 'c';
            qreal x = colX[c];
            if (how == 'c')
                x += (colW[c] - n.kids[i].w) / 2;
            else if (how == 'r')
                x += colW[c] - n.kids[i].w;
            n.kids[i].x = x;
            n.kids[i].y = rowY[r];
        }
        const qreal pad = (!n.left.isNull() && n.left != '.') || (!n.right.isNull() && n.right != '.')
                              ? em * 0.55
                              : 0;
        const qreal side = em * 0.15;
        for (Node &k : n.kids)
            k.x += pad + side;
        n.w = width + pad * 2 + side * 2;
        const qreal shift = (y / 2) - em * 0.2;
        for (Node &k : n.kids)
            k.y -= shift;
        n.a = n.d = 0;
        for (const Node &k : n.kids) {
            n.a = qMax(n.a, k.y + k.a);
            n.d = qMax(n.d, -k.y + k.d);
        }
        n.a += em * 0.08;
        n.d += em * 0.08;
        break;
    }
    case Node::Fence: {
        if (n.text == "delim" || n.text == "box") {
            if (!n.kids.isEmpty())
                layout(n.kids[0], base);
            const Node &body = n.kids.value(0);
            const qreal side = n.text == "box" ? em * 0.35 : em * 0.5;
            if (!n.kids.isEmpty()) {
                n.kids[0].x = side;
                n.kids[0].y = 0;
            }
            n.w = body.w + side * (n.text == "box" ? 2 : (n.left == '.' ? 0.3 : 1) + (n.right == '.' ? 0.3 : 1));
            if (n.text == "delim")
                n.w = body.w + (n.left == '.' ? em * 0.15 : side) + (n.right == '.' ? em * 0.15 : side);
            n.a = body.a + em * 0.08;
            n.d = body.d + em * 0.08;
            break;
        }
        // Scripts stored as a fence: kids are base, sup, sub.
        for (Node &k : n.kids)
            layout(k, base);
        const Node &body = n.kids.value(0);
        const Node sup = n.kids.value(1);
        const Node sub = n.kids.value(2);
        const bool above = n.limits && (n.style == StyDisplay || n.style == StyText);
        const qreal gap = em * 0.06;
        if (above) {
            const qreal supY = body.a + gap + sup.d;
            const qreal subY = -(body.d + gap + sub.a);
            n.w = qMax(body.w, qMax(sup.w, sub.w));
            if (n.kids.size() > 0) { n.kids[0].x = (n.w - body.w) / 2; n.kids[0].y = 0; }
            if (n.kids.size() > 1) { n.kids[1].x = (n.w - sup.w) / 2; n.kids[1].y = supY; }
            if (n.kids.size() > 2) { n.kids[2].x = (n.w - sub.w) / 2; n.kids[2].y = subY; }
        } else {
            const qreal scriptW = qMax(sup.w, sub.w);
            const qreal supY = qMax(body.a - sup.d * 0.15, em * 0.45);
            const qreal subY = -qMax(body.d + sub.a * 0.1, em * 0.2);
            n.w = body.w + gap + scriptW;
            if (n.kids.size() > 0) { n.kids[0].x = 0; n.kids[0].y = 0; }
            if (n.kids.size() > 1) { n.kids[1].x = body.w + gap; n.kids[1].y = supY; }
            if (n.kids.size() > 2) { n.kids[2].x = body.w + gap; n.kids[2].y = subY; }
        }
        n.a = body.a;
        n.d = body.d;
        for (const Node &k : n.kids) {
            n.a = qMax(n.a, k.y + k.a);
            n.d = qMax(n.d, -k.y + k.d);
        }
        break;
    }
    }
}

void paintAt(QPainter *p, const Node &n, qreal x, qreal baseline, qreal base, const QColor &color);

void drawDelim(QPainter *p, QChar kind, const QRectF &rect, const QColor &color, qreal em) {
    if (kind.isNull() || kind == '.')
        return;
    p->save();
    QPen pen(color, qMax(1.0, em * 0.045));
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    const qreal l = rect.left(), t = rect.top(), r = rect.right(), b = rect.bottom();
    const qreal w = rect.width(), h = rect.height();
    QPainterPath path;
    if (kind == '(' || kind == ')') {
        const bool open = kind == '(';
        path.moveTo(open ? r : l, t);
        path.cubicTo(open ? l : r, t + h * 0.25, open ? l : r, t + h * 0.75, open ? r : l, b);
        p->drawPath(path);
    } else if (kind == '[' || kind == ']' || kind == 'L' || kind == 'J' || kind == 'l' || kind == 'r') {
        const bool left = kind == '[' || kind == 'L' || kind == 'l';
        const qreal x = left ? l + w * 0.55 : l + w * 0.45;
        p->drawLine(QPointF(x, t), QPointF(x, b));
        if (kind == '[' || kind == ']') {
            p->drawLine(QPointF(x, t), QPointF(left ? r : l, t));
            p->drawLine(QPointF(x, b), QPointF(left ? r : l, b));
        } else if (kind == 'L' || kind == 'l')
            p->drawLine(QPointF(x, kind == 'L' ? b : t), QPointF(r, kind == 'L' ? b : t));
        else
            p->drawLine(QPointF(x, kind == 'J' ? b : t), QPointF(l, kind == 'J' ? b : t));
    } else if (kind == '{' || kind == '}') {
        const bool open = kind == '{';
        const qreal mid = (t + b) / 2;
        const qreal outer = open ? r : l;
        const qreal inner = open ? l : r;
        path.moveTo(outer, t);
        path.cubicTo((outer + inner) / 2, t, inner, t + h * 0.15, inner, mid);
        path.cubicTo(inner, b - h * 0.15, (outer + inner) / 2, b, outer, b);
        p->drawPath(path);
    } else if (kind == '|' || kind == QChar(0x2016)) {
        p->drawLine(QPointF((l + r) / 2, t), QPointF((l + r) / 2, b));
        if (kind == QChar(0x2016)) {
            p->drawLine(QPointF(l + w * 0.2, t), QPointF(l + w * 0.2, b));
            p->drawLine(QPointF(l + w * 0.8, t), QPointF(l + w * 0.8, b));
        }
    } else if (kind == QChar(0x27E8) || kind == QChar(0x27E9) || kind == '<' || kind == '>') {
        const bool open = kind == QChar(0x27E8) || kind == '<';
        path.moveTo(open ? r : l, t);
        path.lineTo(open ? l : r, (t + b) / 2);
        path.lineTo(open ? r : l, b);
        p->drawPath(path);
    }
    p->restore();
}

void paintAt(QPainter *p, const Node &n, qreal x, qreal baseline, qreal base, const QColor &color) {
    const qreal em = emOf(n, base);
    switch (n.type) {
    case Node::Empty:
    case Node::Space:
        break;
    case Node::Glyph: {
        const bool fakeItalic = n.alphabet == 8 || (n.alphabet == 0 && !mathAlphabetAvailable());
        QFont font = mathFont(n.big ? em * (n.style == StyDisplay ? 1.6 : 1.15) : em, fakeItalic);
        if (n.bold)
            font.setBold(true);
        // addText records outlines in user space, so the slide's painter transform
        // scales them with the surrounding text, including in the PDF.
        QPainterPath path;
        path.addText(x, baseline, font, n.text);
        p->fillPath(path, color);
        break;
    }
    case Node::Row:
    case Node::Stack:
    case Node::Matrix:
        for (const Node &k : n.kids)
            paintAt(p, k, x + k.x, baseline - k.y, base, color);
        if (n.type == Node::Matrix) {
            const qreal top = baseline - n.a, bot = baseline + n.d;
            if (!n.left.isNull())
                drawDelim(p, n.left, QRectF(x + em * 0.08, top, em * 0.42, bot - top), color, em);
            if (!n.right.isNull())
                drawDelim(p, n.right, QRectF(x + n.w - em * 0.5, top, em * 0.42, bot - top), color, em);
        }
        break;
    case Node::Frac: {
        for (const Node &k : n.kids)
            paintAt(p, k, x + k.x, baseline - k.y, base, color);
        if (n.text != "atop" && n.text != "binom") {
            const qreal axis = em * 0.22;
            const qreal rule = qMax(em * 0.045, 0.6);
            p->fillRect(QRectF(x + em * 0.04, baseline - axis - rule / 2, n.w - em * 0.08, rule), color);
        }
        if (n.text == "binom") {
            const qreal top = baseline - n.a, bot = baseline + n.d;
            drawDelim(p, '(', QRectF(x, top, em * 0.4, bot - top), color, em);
            drawDelim(p, ')', QRectF(x + n.w - em * 0.4, top, em * 0.4, bot - top), color, em);
        }
        break;
    }
    case Node::Root: {
        for (const Node &k : n.kids)
            paintAt(p, k, x + k.x, baseline - k.y, base, color);
        const Node &body = n.kids.value(0);
        const qreal surdW = em * 0.55;
        const qreal left = x + body.x - surdW - em * 0.06;
        const qreal top = baseline - n.a + em * 0.02;
        const qreal bot = baseline + body.d;
        const qreal h = qMax(1.0, bot - top);
        QPainterPath path;
        path.moveTo(left + surdW * 0.05, top + h * 0.55);
        path.lineTo(left + surdW * 0.35, top + h * 0.62);
        path.lineTo(left + surdW * 0.55, bot);
        path.lineTo(left + surdW, top);
        path.lineTo(x + n.w - em * 0.02, top);
        QPen pen(color, qMax(1.0, em * 0.045));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p->save();
        p->setPen(pen);
        p->drawPath(path);
        p->restore();
        break;
    }
    case Node::Accent: {
        for (const Node &k : n.kids)
            paintAt(p, k, x + k.x, baseline - k.y, base, color);
        const Node &body = n.kids.value(0);
        const qreal mid = x + body.x + body.w / 2;
        const qreal y = baseline - body.a - em * 0.06;
        p->save();
        QPen pen(color, qMax(1.0, em * 0.04));
        pen.setCapStyle(Qt::RoundCap);
        p->setPen(pen);
        const qreal hw = qMin(body.w, em * (n.text.startsWith("wide") || n.text == "overrightarrow" ? 0.9 : 0.45)) / 2;
        if (n.text == "not") {
            p->drawLine(QPointF(x + em * 0.05, baseline + n.d * 0.2),
                        QPointF(x + n.w - em * 0.05, baseline - n.a * 0.85));
        } else if (n.text == "bar" || n.text == "overline") {
            p->drawLine(QPointF(mid - hw, y), QPointF(mid + hw, y));
        } else if (n.text == "underline") {
            p->drawLine(QPointF(x, baseline + n.d - em * 0.06), QPointF(x + n.w, baseline + n.d - em * 0.06));
        } else if (n.text == "hat" || n.text == "widehat") {
            p->drawLine(QPointF(mid - hw, y + em * 0.12), QPointF(mid, y - em * 0.08));
            p->drawLine(QPointF(mid, y - em * 0.08), QPointF(mid + hw, y + em * 0.12));
        } else if (n.text == "vec" || n.text == "overrightarrow" || n.text == "overleftarrow") {
            const bool left = n.text == "overleftarrow";
            p->drawLine(QPointF(mid - hw, y), QPointF(mid + hw, y));
            const qreal tip = left ? mid - hw : mid + hw;
            const qreal dir = left ? 1 : -1;
            p->drawLine(QPointF(tip, y), QPointF(tip + dir * em * 0.12, y - em * 0.08));
            p->drawLine(QPointF(tip, y), QPointF(tip + dir * em * 0.12, y + em * 0.08));
        } else if (n.text == "dot" || n.text == "ddot") {
            p->setBrush(color);
            p->drawEllipse(QPointF(mid + (n.text == "ddot" ? em * 0.08 : 0), y), em * 0.035, em * 0.035);
            if (n.text == "ddot")
                p->drawEllipse(QPointF(mid - em * 0.08, y), em * 0.035, em * 0.035);
        } else if (n.text == "tilde" || n.text == "widetilde") {
            QPainterPath tilde;
            tilde.moveTo(mid - hw, y);
            tilde.cubicTo(mid - hw / 3, y - em * 0.14, mid + hw / 3, y + em * 0.14, mid + hw, y);
            p->drawPath(tilde);
        }
        p->restore();
        break;
    }
    case Node::Fence:
        if (n.text == "delim" || n.text == "box") {
            for (const Node &k : n.kids)
                paintAt(p, k, x + k.x, baseline - k.y, base, color);
            const qreal top = baseline - n.a, bot = baseline + n.d;
            if (n.text == "box") {
                p->save();
                p->setPen(QPen(color, qMax(1.0, em * 0.04)));
                p->drawRect(QRectF(x + em * 0.04, top, n.w - em * 0.08, bot - top));
                p->restore();
            } else {
                drawDelim(p, n.left, QRectF(x, top, em * 0.42, bot - top), color, em);
                drawDelim(p, n.right, QRectF(x + n.w - em * 0.42, top, em * 0.42, bot - top), color, em);
            }
        } else {
            for (const Node &k : n.kids)
                paintAt(p, k, x + k.x, baseline - k.y, base, color);
        }
        break;
    }
}

Node fallbackNode(const QString &tex) {
    Node n;
    n.type = Node::Glyph;
    n.text = tex;
    n.cls = Ord;
    n.style = StyText;
    n.alphabet = 8;
    return n;
}

Node layoutFormula(const QString &tex, qreal fontSize, bool display, bool *ok) {
    Parser parser;
    parser.s = tex;
    parser.style = display ? StyDisplay : StyText;
    Node node = parser.parseRows(false, false);
    parser.skip();
    const bool parsed = parser.ok && parser.i >= parser.s.size();
    if (ok)
        *ok = parsed;
    if (!parsed || node.type == Node::Empty)
        node = fallbackNode(tex);
    layout(node, fontSize);
    // Italic overhang and a radical bar need a sliver of padding so they aren't clipped.
    node.w += fontSize * 0.08;
    node.a += fontSize * 0.04;
    return node;
}

qreal fontSizeOf(const QTextFormat &format) {
    qreal size = format.property(QTextFormat::FontPixelSize).toDouble();
    if (size <= 0)
        size = format.toCharFormat().font().pixelSize();
    return size > 0 ? size : 32;
}

// The strut of the surrounding text. Inline math reports this height so a
// superscript cannot stretch the line, and draws on the same baseline.
struct TextStrut {
    qreal ascent = 1, descent = 0;
};

TextStrut textStrut(const QTextFormat &format, qreal size) {
    QFont font = format.toCharFormat().font();
    if (font.pixelSize() <= 0)
        font.setPixelSize(qMax(1, qRound(size)));
    const QFontMetricsF fm(font);
    return {qMax(1.0, fm.ascent()), qMax(0.0, fm.descent())};
}

// A superscript or a shallow fraction fits in the gap between lines. A root
// is taller than that, so it shrinks until its bar stays in the gap.
qreal inlineScale(const Node &node, const TextStrut &strut, qreal size) {
    const qreal budgetAscent = strut.ascent + size * 0.50;
    const qreal budgetDescent = strut.descent + size * 0.32;
    qreal scale = 1;
    if (node.a > budgetAscent)
        scale = qMin(scale, budgetAscent / node.a);
    if (node.d > budgetDescent)
        scale = qMin(scale, budgetDescent / node.d);
    return scale;
}

class MathHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &format) override {
        const qreal size = fontSizeOf(format);
        const bool display = format.property(HypeMathDisplay).toBool();
        const Node node = layoutFormula(format.property(HypeMathTex).toString(), size, display, nullptr);
        qreal height = qMax(1.0, node.a + node.d);
        qreal width = node.w;
        if (!display) {
            const TextStrut strut = textStrut(format, size);
            height = strut.ascent + strut.descent;
            width *= inlineScale(node, strut, size);
        }
        return QSizeF(qMax(1.0, width), height);
    }
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *, int,
                    const QTextFormat &format) override {
        const qreal size = fontSizeOf(format);
        const bool display = format.property(HypeMathDisplay).toBool();
        const Node node = layoutFormula(format.property(HypeMathTex).toString(), size, display, nullptr);
        QColor color = format.foreground().color();
        if (!color.isValid())
            color = Qt::black;
        // A displayed equation fills its own rect. An inline equation shares the
        // text baseline: AlignBaseline makes Qt split the strut into ascent and
        // descent, so the baseline sits `descent` above the rect bottom.
        qreal baseline = rect.top() + node.a;
        qreal scale = 1;
        if (!display) {
            const TextStrut strut = textStrut(format, size);
            baseline = rect.bottom() - strut.descent;
            scale = inlineScale(node, strut, size);
        }
        painter->save();
        painter->translate(rect.left(), baseline);
        painter->scale(scale, scale);
        paintAt(painter, node, 0, 0, size, color);
        painter->restore();
    }
};

} // namespace math_detail

void materializeMath(QTextDocument &document, const QVector<MathSpan> &spans, const QColor &color) {
    if (spans.isEmpty())
        return;
    static math_detail::MathHandler *handler = new math_detail::MathHandler;
    document.documentLayout()->registerHandler(HypeMathObject, handler);
    const QString text = document.toPlainText();
    struct Hit {
        int pos, length, index;
    };
    QVector<Hit> hits;
    for (int i = 0; i < text.size();) {
        if (text[i] != QChar(0xE000)) {
            ++i;
            continue;
        }
        const int end = text.indexOf(QChar(0xE001), i + 1);
        if (end < 0)
            break;
        const QString body = text.mid(i + 1, end - i - 1);
        if (body.startsWith("HYPEMATH")) {
            bool ok = false;
            const int index = body.mid(8).toInt(&ok);
            if (ok && index >= 0 && index < spans.size())
                hits.append({i, end + 1 - i, index});
        }
        i = end + 1;
    }
    for (int h = hits.size() - 1; h >= 0; --h) {
        QTextCursor cursor(&document);
        cursor.setPosition(hits[h].pos);
        cursor.setPosition(hits[h].pos + hits[h].length, QTextCursor::KeepAnchor);
        QTextCharFormat format = cursor.charFormat();
        format.setObjectType(HypeMathObject);
        format.setProperty(HypeMathTex, spans[hits[h].index].tex);
        format.setProperty(HypeMathDisplay, spans[hits[h].index].display);
        if (!spans[hits[h].index].display)
            format.setVerticalAlignment(QTextCharFormat::AlignBaseline);
        format.setForeground(color);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
    }
}

#include "math.moc"
