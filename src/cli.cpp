#include "cli.h"
#include "deck.h"
#include "templates.h"
#include "renderer.h"
#include "blocks.h"
#include "fonts.h"
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
    paintSlide(&painter, image.rect(), deck.slide(index), deck.baseDir(), deck.slidePalette(index), warning);
    return image;
}
QList<Problem> slideProblemsAt(const Deck &deck, const ParsedDeck &parsed, int index) {
    QList<Problem> problems;
    const int line = firstLine(deck.source(), parsed.slides[index]);
    for (const auto &message : slideProblems(deck.slide(index), deck.baseDir(), deck.slidePalette(index)))
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
    // A font that could not be found or fetched falls back to another; say so, and where.
    for (const QString &family : deck.missingFonts()) {
        const QString pattern = "font\\s*=\\s*[\"']" + QRegularExpression::escape(family) + "[\"']";
        int slide = 0, line = 0;
        for (int i = 0; i < parsed.slides.size() && !slide; ++i)
            if (const auto m = QRegularExpression(pattern).match(parsed.slides[i].source); m.hasMatch()) {
                slide = i + 1;
                line = lineAt(source, parsed.slides[i].start + m.capturedStart());
            }
        if (!slide)
            line = lineAt(source, qMax(0, parsed.header.indexOf(family)));
        QString reason = fonts::downloadsAllowed() ? "is not installed and could not be fetched from Google Fonts"
                                                   : "is not installed (font downloads are off)";
        if (const QStringList near = fonts::suggestions(family); !near.isEmpty() && !near.contains(family))
            reason += "; did you mean " + near.join(", ") + "?";
        problems << Problem{slide, line, false, "Font " + family + " " + reason + "; text uses a fallback"};
    }
    if (!scalar(parsed.header, "theme").isEmpty() && !deck.themeNames().contains(deck.themeName()) &&
        scalar(parsed.header, "color_background").isEmpty())
        problems << Problem{0, headerLine("theme"), false,
                            "Theme " + deck.themeName() + " is not installed; using the default colors"};
    const QString templateError = deck.palette().value("template_error").toString();
    if (!templateError.isEmpty())
        for (const QString &line : templateError.split('\n'))
            problems << Problem{0, headerLine("template"), true, line};
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
        // Fonts the presentation names but this machine lacks come from Google Fonts first,
        // so text is measured and drawn in the font the author chose.
        fontProblems = deck.fetchFonts();
        if (reportFonts)
            for (const QString &problem : fontProblems)
                print(stderr, "warning: " + problem);
        return true;
    }
    mutable QStringList fontProblems;
    bool reportFonts = true; // check and fonts report missing fonts themselves.
};

int check(const QStringList &arguments) {
    Command command("check", "Report every problem in a presentation, with its slide and line.");
    command.json();
    command.parser.process(arguments);
    command.reportFonts = false;
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
    command.parser.addOption({"template", "Template: a name in templates/ or ~/.local/share/hype/templates/, "
                                          "or a folder or presentation to build on",
                              "template"});
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
        return fail("Theme " + theme + " is not installed. Installed: " + deck.themeNames().join(", "));
    QString font = command.parser.value("font");
    if (!font.isEmpty() && font != deck.fontName() && !fonts::available(font)) {
        QString name;
        if (const QString error = fonts::download(font, &name); !error.isEmpty())
            return fail("Font " + font + ": " + error);
        fonts::loadDirectory(fonts::downloadedDirectory(name));
        print(stderr, "Fetched " + name + " from Google Fonts");
        font = name;
    }
    const QString templateName = command.parser.value("template");
    if (!templateName.isEmpty()) {
        const TemplatePack pack = loadTemplate(templateName, QFileInfo(path).absolutePath());
        if (pack.path.isEmpty())
            return fail(pack.error);
    }
    // Choosing a theme also records its colors, so the file renders the same anywhere.
    // A template brings its own colors instead.
    if (templateName.isEmpty() || !theme.isEmpty())
        deck.chooseTheme(theme.isEmpty() ? deck.themeName() : theme);
    if (!font.isEmpty() && font != deck.fontName())
        deck.editSource(setScalar(parseDeck(deck.source()).header, "font", font) +
                        deck.source().mid(parseDeck(deck.source()).header.size()));
    // talk/presentation.md is named for its folder.
    const QFileInfo named(path);
    const QString title = command.parser.isSet("title") ? command.parser.value("title")
                          : named.fileName() == "presentation.md" ? named.absoluteDir().dirName()
                                                                  : named.completeBaseName();
    auto parsed = parseDeck(deck.source());
    QString header = setScalar(parsed.header, "title", title);
    if (!templateName.isEmpty()) {
        if (theme.isEmpty())
            header = setScalar(header, "theme", "");
        header.remove(QRegularExpression("^theme: *\"?\"?\n", QRegularExpression::MultilineOption));
        header = setScalar(header, "template", templateName);
    }
    const QString source = header + "\n# " + title + "\n";
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
    Command command("themes", "List the installed themes.", false);
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

QJsonObject toJson(const templates::Entry &entry) {
    return {{"name", entry.name},         {"title", entry.title},     {"where", entry.where},
            {"path", entry.path},         {"linked", entry.linked.isEmpty() ? QJsonValue() : QJsonValue(entry.linked)},
            {"theme", entry.theme.isEmpty() ? QJsonValue() : QJsonValue(entry.theme)},
            {"template", entry.buildsOn.isEmpty() ? QJsonValue() : QJsonValue(entry.buildsOn)},
            {"components", QJsonArray::fromStringList(entry.components)}, {"slides", entry.slides},
            {"starters", [&] {
                 QJsonArray list;
                 for (int i = 0; i < entry.starters.size(); ++i)
                     list << QJsonObject{{"slide", i + 1}, {"label", entry.starters[i]}, {"section", entry.sections[i]}};
                 return list;
             }()}};
}

int templatesCommand(const QStringList &arguments) {
    Command command("templates", "List the templates a presentation can name with template:.", false);
    command.parser.addPositionalArgument("presentation", "Also list templates/ beside this presentation", "[presentation]");
    command.json();
    command.parser.process(arguments);
    const QString presentation = command.argument(1);
    const QString base = presentation.isEmpty() ? QDir::currentPath() : QFileInfo(presentation).absolutePath();
    const auto found = templates::available(base);
    if (command.parser.isSet("json")) {
        QJsonArray list;
        for (const auto &entry : found)
            list << toJson(entry);
        print({{"templates", list}, {"library", templates::libraryDirectory()}});
        return 0;
    }
    for (const auto &entry : found) {
        QStringList details{entry.where};
        if (!entry.buildsOn.isEmpty())
            details << "builds on " + entry.buildsOn;
        details << QString::number(entry.components.size()) + " components";
        if (entry.slides)
            details << QString::number(entry.slides) + " slides";
        print(stdout, QString("%1  %2  (%3)").arg(entry.name, -16).arg(entry.title, details.join(", ")));
        print(stdout, QString(18, ' ') + (entry.linked.isEmpty() ? entry.path : "follows " + entry.linked));
    }
    if (found.isEmpty())
        print(stdout, "No templates yet. Save a presentation as one with: hype template save <presentation>");
    return 0;
}

int templateCommand(const QStringList &arguments) {
    Command command("template", "Add a presentation to the template library, remove one, or list its slides:\n"
                                "  hype template save <presentation> [--name acme] [--copy]\n"
                                "  hype template remove <name>\n"
                                "  hype template slides <template> [slide]   the slides to copy, or one slide's Markdown", false);
    command.parser.addPositionalArgument("action", "save, remove or slides", "save|remove|slides");
    command.parser.addPositionalArgument("target", "The presentation to save, the template to remove or list", "<target>");
    command.parser.addOption({"name", "The name presentations use for it (default: the file or folder's name)", "name"});
    command.parser.addOption({"copy", "Keep a copy as it is now, instead of a link that follows the presentation"});
    command.parser.addOption({"force", "Replace a template of the same name"});
    command.json();
    command.parser.process(arguments);
    const QString action = command.argument(1), target = command.argument(2);
    const bool json = command.parser.isSet("json");
    if (action == "slides") {
        if (target.isEmpty())
            return fail("Name the template: hype template slides omarchy");
        // A name, folder or presentation, found as template: finds it from here.
        const TemplatePack pack = loadTemplate(target, QDir::currentPath());
        if (pack.path.isEmpty())
            return fail(pack.error);
        const auto starters = templates::starterSlides(pack.path);
        if (!command.argument(3).isEmpty()) {
            bool ok = false;
            const int n = command.argument(3).toInt(&ok);
            if (!ok || n < 1 || n > starters.size())
                return fail(QString("Choose a slide from 1 to %1").arg(starters.size()));
            const auto &starter = starters[n - 1];
            if (json)
                print({{"slide", n}, {"label", starter.label}, {"section", starter.section}, {"source", starter.source.trimmed()}});
            else
                print(stdout, starter.source.trimmed());
            return 0;
        }
        if (json) {
            QJsonArray list;
            for (int i = 0; i < starters.size(); ++i)
                list << QJsonObject{{"slide", i + 1}, {"label", starters[i].label}, {"section", starters[i].section},
                                    {"source", starters[i].source.trimmed()}};
            print({{"template", pack.path}, {"slides", list}});
            return 0;
        }
        QString section = "\x01";
        for (int i = 0; i < starters.size(); ++i) {
            if (starters[i].section != section) {
                section = starters[i].section;
                print(stdout, (i ? "\n" : "") + (section.isEmpty() ? QString("(no section)") : section));
            }
            print(stdout, QString("%1  %2").arg(i + 1, 4).arg(starters[i].label));
        }
        return 0;
    }
    if (action == "save") {
        if (target.isEmpty())
            return fail("Name the presentation to save: hype template save talk/presentation.md");
        Deck deck;
        if (!deck.loadPath(target, false))
            return fail(target + ": " + deck.status());
        QString saved;
        const bool copy = command.parser.isSet("copy");
        if (const QString error = templates::save(target, command.parser.value("name"), copy,
                                                  command.parser.isSet("force"), &saved);
            !error.isEmpty())
            return fail(error);
        const QString name = QFileInfo(saved).fileName();
        if (json)
            print({{"name", name}, {"path", saved}, {"copy", copy},
                   {"presentation", QFileInfo(target).absoluteFilePath()}});
        else
            print(stdout, (copy ? "Copied " : "Linked ") + target + " as the template " + name +
                              ". Presentations use it with template: " + name);
        return 0;
    }
    if (action == "remove") {
        QString removed;
        if (const QString error = templates::remove(target, &removed); !error.isEmpty())
            return fail(error);
        if (json)
            print({{"removed", target}});
        else
            print(stdout, removed);
        return 0;
    }
    return fail("Use hype template save <presentation> or hype template remove <name>.");
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

int fontsCommand(const QStringList &arguments) {
    Command command("fonts", "Show the fonts a presentation uses, or add and search Google Fonts.", false);
    command.parser.addPositionalArgument("what", "A presentation, add <family>…, or search <words>",
                                         "<presentation> | add <family>… | search [words]");
    command.parser.addOption({"into", "With add: copy the fonts into fonts/ beside this presentation", "presentation"});
    command.json();
    command.parser.process(arguments);
    const QStringList words = command.parser.positionalArguments().mid(1);
    const bool json = command.parser.isSet("json");
    if (words.value(0) == "search") {
        const QStringList names = fonts::search(words.mid(1).join(' '), 40);
        if (fonts::catalog(false).isEmpty())
            return fail("Could not load the Google Fonts catalog.");
        if (json) {
            QJsonArray list;
            for (const auto &family : fonts::catalog(false))
                if (names.contains(family.name))
                    list << QJsonObject{{"family", family.name}, {"category", family.category},
                                        {"styles", QJsonArray::fromStringList(family.styles)}};
            print({{"fonts", list}});
        } else if (names.isEmpty())
            return fail("No Google Fonts family matches " + words.mid(1).join(' ') + ".");
        else
            print(stdout, names.join('\n'));
        return 0;
    }
    if (words.value(0) == "add") {
        if (words.size() < 2)
            return fail("Name a Google Fonts family: hype fonts add \"Encode Sans\"");
        QString into;
        if (command.parser.isSet("into")) {
            into = QFileInfo(command.parser.value("into")).absoluteDir().filePath("fonts");
            if (!QDir().mkpath(into))
                return fail("Could not create " + into);
        }
        QJsonArray added;
        for (const QString &family : words.mid(1)) {
            QString name = family;
            if (const QString error = fonts::download(family, &name); !error.isEmpty())
                return fail("Font " + family + ": " + error);
            const QDir from(fonts::downloadedDirectory(name));
            QStringList files = from.entryList({"*.ttf", "*.otf", "*.txt"}, QDir::Files);
            for (const QString &file : files) {
                if (into.isEmpty())
                    continue;
                // A license file names its family, so two families never overwrite each other's.
                const QString target = file.endsWith(".txt") ? QString(name).remove(' ') + "-" + file : file;
                QFile::remove(QDir(into).filePath(target));
                if (!QFile::copy(from.filePath(file), QDir(into).filePath(target)))
                    return fail("Could not copy " + file + " into " + into);
            }
            added << QJsonObject{{"family", name}, {"directory", into.isEmpty() ? from.path() : into},
                                 {"files", QJsonArray::fromStringList(files)}};
            if (!json)
                print(stdout, "Added " + name + " (" + QString::number(files.filter(".ttf").size()) + " styles) to " +
                                  (into.isEmpty() ? from.path() : into));
        }
        if (json)
            print({{"added", added}});
        return 0;
    }
    if (words.isEmpty())
        return fail("Name a presentation, or use hype fonts add <family> or hype fonts search <words>.");
    Command load("fonts", "", true);
    load.parser.process({arguments[0], "fonts", words[0]});
    load.reportFonts = false;
    Deck deck;
    if (!load.load(deck))
        return 1;
    // Where each family the presentation names comes from.
    const QVariantMap palette = deck.palette();
    QStringList bundled;
    QStringList folders{deck.baseDir() + "/fonts"};
    for (const QString &directory : palette.value("template_dirs").toString().split('\n', Qt::SkipEmptyParts))
        folders << directory + "/fonts";
    for (const QString &folder : folders)
        for (const QString &file : QDir(folder).entryList({"*.ttf", "*.otf"}, QDir::Files))
            bundled << QFileInfo(folder + "/" + file).absoluteFilePath();
    QStringList families{palette.value("font").toString()};
    families += deck.requestedFonts();
    families.removeDuplicates();
    QJsonArray list;
    for (const QString &family : families) {
        QString where = "installed";
        const QString compact = QString(family).remove(' ').toLower();
        for (const QString &file : bundled)
            if (QFileInfo(file).fileName().remove(' ').toLower().startsWith(compact))
                where = QDir(deck.baseDir()).relativeFilePath(QFileInfo(file).absolutePath());
        if (where == "installed" && QFileInfo::exists(fonts::downloadedDirectory(family)))
            where = "Google Fonts (" + fonts::downloadedDirectory(family) + ")";
        if (!fonts::available(family))
            where = "missing";
        list << QJsonObject{{"family", family}, {"source", where}};
        if (!json)
            print(where == "missing" ? stderr : stdout, family + ": " + where);
    }
    for (const QString &problem : load.fontProblems)
        print(stderr, problem);
    if (json)
        print({{"fonts", list}});
    return deck.missingFonts().isEmpty() ? 0 : 1;
}

int icons(const QStringList &arguments) {
    Command command("icons", "Search the bundled Lucide icons by name or keyword.", false);
    command.parser.addPositionalArgument("query", "Words to search for, such as chart or user", "[query...]");
    command.parser.addOption({"limit", "Most names to list (default: 40; 0 for all)", "count", "40"});
    command.json();
    command.parser.process(arguments);
    const QStringList words = command.parser.positionalArguments().mid(1);
    const QStringList names = searchIcons(words.join(' '), command.parser.value("limit").toInt());
    if (command.parser.isSet("json"))
        print({{"icons", QJsonArray::fromStringList(names)}});
    else if (names.isEmpty())
        return fail("No icon matches " + words.join(' ') + ".");
    else
        print(stdout, names.join('\n'));
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (topic == "components") {
        fputs(componentGuide().toUtf8().constData(), stdout);
        return 0;
    }
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
           "  templates [presentation]        List the templates a presentation can use\n"
           "  template save <presentation>    Add a presentation to the template library\n"
           "  template remove <name>          Remove a template from the library\n"
           "  icons [query]                   Search icons for components\n"
           "  fonts <presentation>            Show where each font comes from\n"
           "  fonts add <family>… [--into p]  Add Google Fonts, optionally into a presentation\n"
           "  fonts search [words]            Search Google Fonts\n"
           "  help format                     How to write a presentation\n"
           "  help components                 Layouts, cards, charts, diagrams and templates\n"
           "  skill [install]                 Print the skill for coding agents, or install it";
}
bool isCliCommand(const QString &word) {
    return QStringList{"new", "check", "slides", "render", "export", "themes", "templates", "template", "icons",
                       "fonts", "skill", "help"}
        .contains(word);
}
int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "new"      ? create(arguments)
           : command == "check"  ? check(arguments)
           : command == "slides" ? slides(arguments)
           : command == "render" ? render(arguments)
           : command == "export" ? exportDeck(arguments)
           : command == "themes" ? themes(arguments)
           : command == "templates" ? templatesCommand(arguments)
           : command == "template" ? templateCommand(arguments)
           : command == "icons"  ? icons(arguments)
           : command == "fonts"  ? fontsCommand(arguments)
           : command == "skill"  ? skill(arguments)
                                 : help(arguments);
}
