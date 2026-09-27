#include "cli.h"
#include "deck.h"
#include "renderer.h"
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtConcurrentMap>
#include <cstdio>

namespace {
struct Problem {
    int slide = 0; // 1-based; 0 for the presentation as a whole.
    int line = 1;
    bool error = true;
    QString message;
};
void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(text)); }
void print(const QJsonObject &object) {
    fprintf(stdout, "%s", QJsonDocument(object).toJson().constData());
}
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}
int lineAt(const QString &source, int offset) { return source.left(offset).count('\n') + 1; }
// A slide starts on the blank line after its separator; point at its content,
// or at the separator when there is none.
int firstLine(const QString &source, const Slide &slide) {
    int offset = slide.start;
    while (offset < slide.end && source[offset].isSpace())
        ++offset;
    return lineAt(source, offset < slide.end ? offset : qMax(0, slide.start - 1));
}
int lastLine(const QString &source, const Slide &slide) {
    int offset = slide.end;
    while (offset > slide.start && source[offset - 1].isSpace())
        --offset;
    return lineAt(source, offset);
}
QImage paint(const Deck &deck, int index, int width, QString *warning) {
    QImage image(width, width * 9 / 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintSlide(&painter, image.rect(), deck.slide(index), deck.baseDir(), deck.palette(), warning);
    return image;
}
QList<Problem> slideProblemsAt(const Deck &deck, const ParsedDeck &parsed, int index) {
    QList<Problem> problems;
    const int line = firstLine(deck.source(), parsed.slides[index]);
    for (const auto &message : slideProblems(deck.slide(index), deck.baseDir()))
        problems << Problem{index + 1, line, true, message};
    if (deck.slide(index).trimmed().isEmpty())
        problems << Problem{index + 1, line, false, "Empty slide"};
    return problems;
}
QList<Problem> findProblems(const Deck &deck) {
    const QString source = deck.source();
    const auto parsed = parseDeck(source);
    // Slide boundaries are unreliable past unfinished front matter or a code fence.
    if (!parsed.error.isEmpty())
        return {Problem{0, lineAt(source, parsed.errorOffset), true, parsed.error}};
    QList<Problem> problems;
    auto headerLine = [&](const QString &key) {
        return lineAt(source, qMax(0, parsed.header.indexOf(QRegularExpression(
                                          "^" + key + ":", QRegularExpression::MultilineOption))));
    };
    if (!scalar(parsed.header, "theme").isEmpty() && !deck.themeNames().contains(deck.themeName()) &&
        scalar(parsed.header, "color_background").isEmpty())
        problems << Problem{0, headerLine("theme"), false,
                            "Theme " + deck.themeName() + " is unavailable; using the default colors"};
    QList<int> indices;
    for (int i = 0; i < deck.count(); ++i)
        indices << i;
    // Only painting reveals text that had to shrink too far, so lay out every slide.
    std::function<QList<Problem>(int)> inspect = [&](int index) {
        auto found = slideProblemsAt(deck, parsed, index);
        if (found.isEmpty()) {
            QString warning;
            paint(deck, index, 480, &warning);
            if (!warning.isEmpty())
                found << Problem{index + 1, firstLine(source, parsed.slides[index]), false, warning};
        }
        return found;
    };
    deck.palette(); // Fill the palette cache before the workers share it.
    for (const auto &found : QtConcurrent::blockingMapped<QList<QList<Problem>>>(indices, inspect))
        problems << found;
    return problems;
}
QJsonObject toJson(const Problem &problem) {
    return {{"slide", problem.slide ? QJsonValue(problem.slide) : QJsonValue()},
            {"line", problem.line},
            {"severity", problem.error ? "error" : "warning"},
            {"message", problem.message}};
}
QString toText(const QString &path, const Problem &problem) {
    return QString("%1:%2: %3%4: %5")
        .arg(path)
        .arg(problem.line)
        .arg(problem.slide ? QString("slide %1 ").arg(problem.slide) : QString())
        .arg(problem.error ? "error" : "warning", problem.message);
}
// The headline, or else the first line of text, or else of code. Fences close as in parseDeck.
QString slideTitle(const QString &text) {
    static const QRegularExpression fenceRe("^(`{3,}|~{3,})(.*)$");
    QString first, code, fence;
    for (const auto &raw : text.split('\n')) {
        const QString line = raw.trimmed();
        const auto match = fenceRe.match(line);
        const QString run = match.captured(1);
        if (match.hasMatch() && fence.isEmpty())
            fence = run;
        else if (match.hasMatch() && run[0] == fence[0] && run.size() >= fence.size() &&
                 match.captured(2).trimmed().isEmpty())
            fence.clear();
        else if (!fence.isEmpty() && code.isEmpty())
            code = line;
        else if (fence.isEmpty() && line.startsWith('#'))
            return line.mid(line.indexOf(' ') + 1).trimmed();
        else if (fence.isEmpty() && first.isEmpty())
            first = QString(line).remove(QRegularExpression(R"(^(>|[-*+]|\d+\.)\s+|\\$)")).trimmed();
    }
    return first.isEmpty() ? code : first;
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description, bool presentation = true) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
        if (presentation)
            parser.addPositionalArgument("presentation", "Markdown presentation", "<presentation>");
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
    // Commands never touch the editor's memory of the last presentation.
    bool load(Deck &deck) const {
        if (argument(1).isEmpty()) {
            print(stderr, "Name a Markdown presentation.");
            return false;
        }
        if (!deck.loadPath(argument(1), false)) {
            print(stderr, argument(1) + ": " + deck.status());
            return false;
        }
        return true;
    }
};

int check(const QStringList &arguments) {
    Command command("check", "Report every problem in a presentation, with its slide and line.");
    command.json();
    command.parser.process(arguments);
    Deck deck;
    if (!command.load(deck))
        return 1;
    const auto problems = findProblems(deck);
    int errors = 0;
    for (const auto &problem : problems)
        errors += problem.error;
    if (command.parser.isSet("json")) {
        QJsonArray list;
        for (const auto &problem : problems)
            list << toJson(problem);
        print({{"ok", errors == 0}, {"slides", deck.count()}, {"problems", list}});
        return errors ? 1 : 0;
    }
    for (const auto &problem : problems)
        print(problem.error ? stderr : stdout, toText(command.argument(1), problem));
    const int warnings = problems.size() - errors;
    print(stdout, QString("%1 slides, %2 errors, %3 warnings").arg(deck.count()).arg(errors).arg(warnings));
    return errors ? 1 : 0;
}

int slides(const QStringList &arguments) {
    Command command("slides", "Outline the slides: number, lines, headline, and media.");
    command.json();
    command.parser.process(arguments);
    Deck deck;
    if (!command.load(deck))
        return 1;
    const auto parsed = parseDeck(deck.source());
    QJsonArray list;
    for (int i = 0; i < parsed.slides.size(); ++i) {
        const auto media = parseMedia(parsed.slides[i].source, deck.baseDir());
        const int first = firstLine(deck.source(), parsed.slides[i]);
        const int last = qMax(first, lastLine(deck.source(), parsed.slides[i]));
        const QString title = slideTitle(media.text);
        list << QJsonObject{{"slide", i + 1}, {"line", first}, {"endLine", last}, {"title", title},
                            {"media", media.file.isEmpty() ? QJsonValue() : QJsonValue(media.file)}};
        if (!command.parser.isSet("json"))
            print(stdout, QString("%1  %2  %3%4")
                              .arg(i + 1, 3)
                              .arg(QString("%1-%2").arg(first).arg(last), -9)
                              .arg(title.isEmpty() ? "(no text)" : title,
                                   media.file.isEmpty() ? QString() : "  [" + media.file + "]"));
    }
    if (command.parser.isSet("json"))
        print({{"title", deck.title()}, {"theme", deck.themeName()}, {"font", deck.fontName()}, {"slides", list}});
    return 0;
}

int render(const QStringList &arguments) {
    Command command("render", "Render one slide to a PNG, or every slide to a directory with slides.json.");
    command.parser.addOption({"slide", "Render only this slide (1-based)", "number"});
    command.parser.addOption({{"o", "output"}, "PNG file for one slide, or directory for all (default: "
                                               "slide-NNN.png, or slides/)", "path"});
    command.parser.addOption({"width", "Image width in pixels (default: 1920)", "pixels", "1920"});
    command.json();
    command.parser.process(arguments);
    Deck deck;
    if (!command.load(deck))
        return 1;
    const int width = command.parser.value("width").toInt();
    if (width < 160 || width > 7680)
        return fail("Width must be between 160 and 7680.");
    const bool json = command.parser.isSet("json");
    QString output = command.parser.value("output");
    if (!command.parser.isSet("slide")) {
        if (output.isEmpty())
            output = "slides";
        if (!deck.renderImages(output, width))
            return fail(deck.status());
        if (json)
            print({{"directory", QFileInfo(output).absoluteFilePath()},
                   {"manifest", QFileInfo(output + "/slides.json").absoluteFilePath()},
                   {"slides", deck.count()}});
        else
            print(stdout, QString("Rendered %1 slides to %2").arg(deck.count()).arg(output));
        return 0;
    }
    const int number = command.parser.value("slide").toInt();
    const auto parsed = parseDeck(deck.source());
    if (!parsed.error.isEmpty())
        return fail(QString("%1:%2: %3").arg(command.argument(1)).arg(lineAt(deck.source(), parsed.errorOffset))
                        .arg(parsed.error));
    if (number < 1 || number > deck.count())
        return fail(QString("Slide %1 does not exist; there are %2 slides.").arg(number).arg(deck.count()));
    const QString name = QString("slide-%1.png").arg(number, 3, 10, QChar('0'));
    if (output.isEmpty())
        output = name;
    else if (QFileInfo(output).isDir())
        output = QDir(output).filePath(name);
    QDir().mkpath(QFileInfo(output).absolutePath());
    // A slide with problems still renders, with the problems on a banner, so it can be looked at.
    const auto problems = slideProblemsAt(deck, parsed, number - 1);
    QString warning;
    const QImage image = paint(deck, number - 1, width, &warning);
    if (!image.save(output, "PNG"))
        return fail("Could not save " + output);
    int errors = 0;
    QJsonArray list;
    for (const auto &problem : problems) {
        errors += problem.error;
        list << toJson(problem);
        if (!json)
            print(stderr, toText(command.argument(1), problem));
    }
    if (problems.isEmpty() && !warning.isEmpty()) {
        const Problem problem{number, firstLine(deck.source(), parsed.slides[number - 1]), false, warning};
        list << toJson(problem);
        if (!json)
            print(stderr, toText(command.argument(1), problem));
    }
    if (json)
        print({{"slide", number}, {"image", QFileInfo(output).absoluteFilePath()}, {"width", image.width()},
               {"height", image.height()}, {"problems", list}});
    else
        print(stdout, "Rendered " + output);
    return errors ? 1 : 0;
}

int exportDeck(const QStringList &arguments) {
    Command command("export", "Export a presentation as PDF or PowerPoint, chosen by the file extension.");
    command.parser.addPositionalArgument("output", "File ending in .pdf or .pptx", "<output>");
    command.json();
    command.parser.process(arguments);
    const QString output = command.argument(2);
    const QString format = QFileInfo(output).suffix().toLower();
    if (format != "pdf" && format != "pptx")
        return fail("Name an output file ending in .pdf or .pptx.");
    Deck deck;
    if (!command.load(deck))
        return 1;
    QDir().mkpath(QFileInfo(output).absolutePath());
    if (!(format == "pdf" ? deck.exportPdf(output) : deck.exportPptx(output)))
        return fail(deck.status());
    if (command.parser.isSet("json"))
        print({{"output", QFileInfo(output).absoluteFilePath()}, {"format", format}, {"slides", deck.count()}});
    else
        print(stdout, deck.status());
    return 0;
}

int create(const QStringList &arguments) {
    Command command("new", "Start a presentation, with images/ and videos/ beside it.");
    command.parser.addOption({"title", "Presentation title (default: the file name)", "title"});
    command.parser.addOption({"theme", "Installed theme; see hype themes (default: tokyo-night)", "name"});
    command.parser.addOption({"font", "Installed font family (default: JetBrains Mono)", "family"});
    command.json();
    command.parser.process(arguments);
    const QString path = command.argument(1);
    if (path.isEmpty())
        return fail("Name the Markdown file to create.");
    if (QFileInfo::exists(path))
        return fail(path + " already exists.");
    Deck deck;
    const QString theme = command.parser.value("theme");
    if (!theme.isEmpty() && !deck.themeNames().contains(theme))
        return fail("Theme " + theme + " is unavailable. Available: " + deck.themeNames().join(", "));
    const QString font = command.parser.value("font");
    if (!font.isEmpty() && font != deck.fontName() && !deck.fontNames().contains(font))
        return fail("Font " + font + " is not installed.");
    // Choosing a theme also records its colors, so the file renders the same anywhere.
    deck.chooseTheme(theme.isEmpty() ? deck.themeName() : theme);
    if (!font.isEmpty())
        deck.chooseFont(font);
    const QString title = command.parser.isSet("title") ? command.parser.value("title")
                                                        : QFileInfo(path).completeBaseName();
    const auto parsed = parseDeck(deck.source());
    const QString source = setScalar(parsed.header, "title", title) + "\n# " + title + "\n";
    const QDir directory = QFileInfo(path).absoluteDir();
    if (!directory.mkpath("images") || !directory.mkpath("videos"))
        return fail("Could not create " + directory.path());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(source.toUtf8()) < 0 || !file.commit())
        return fail(path + ": " + file.errorString());
    if (command.parser.isSet("json"))
        print({{"presentation", QFileInfo(path).absoluteFilePath()}, {"title", title}, {"theme", deck.themeName()}});
    else
        print(stdout, "Created " + path);
    return 0;
}

int themes(const QStringList &arguments) {
    Command command("themes", "List the available themes.", false);
    command.json();
    command.parser.process(arguments);
    const Deck deck;
    if (command.parser.isSet("json"))
        print({{"themes", QJsonArray::fromStringList(deck.themeNames())}, {"default", deck.themeName()}});
    else
        for (const auto &name : deck.themeNames())
            print(stdout, name);
    return 0;
}

// As the hey and basecamp tools do: one shared copy, linked into Claude Code, found directly by Codex.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.", false);
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/hype and link into ~/.claude/skills", "[install]");
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return 1;
    const QByteArray text = bundled.readAll();
    if (command.argument(1).isEmpty()) {
        fputs(text.constData(), stdout);
        return 0;
    }
    if (command.argument(1) != "install")
        return fail("Use hype skill to print the skill, or hype skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/hype");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    print(stdout, "Installed " + file.fileName());
    if (!home.exists(".claude"))
        return 0;
    const QString link = home.filePath(".claude/skills/hype");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/hype", link))
        return fail("Could not link " + link);
    print(stdout, "Linked " + link);
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (topic == "format") {
        QFile guide(":/format.md");
        if (!guide.open(QIODevice::ReadOnly))
            return 1;
        fputs(guide.readAll().constData(), stdout);
        return 0;
    }
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: hype <command> [options]\n\n" + cliSummary());
    return 0;
}
} // namespace

QString cliSummary() {
    return "Editor:\n"
           "  open [presentation]             Open the editor, on the last presentation by default\n\n"
           "Commands that need no display (hype help <command> for options):\n"
           "  new <presentation>              Start a presentation\n"
           "  check <presentation>            Report every problem, with slide and line\n"
           "  slides <presentation>           Outline the slides\n"
           "  render <presentation>           Render one slide or all of them to PNG\n"
           "  export <presentation> <output>  Export PDF or PowerPoint\n"
           "  themes                          List installed themes\n"
           "  help format                     How to write a presentation\n"
           "  skill [install]                 Print the skill for coding agents, or install it";
}
bool isCliCommand(const QString &word) {
    return QStringList{"new", "check", "slides", "render", "export", "themes", "skill", "help"}.contains(word);
}
int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "new"      ? create(arguments)
           : command == "check"  ? check(arguments)
           : command == "slides" ? slides(arguments)
           : command == "render" ? render(arguments)
           : command == "export" ? exportDeck(arguments)
           : command == "themes" ? themes(arguments)
           : command == "skill"  ? skill(arguments)
                                 : help(arguments);
}
