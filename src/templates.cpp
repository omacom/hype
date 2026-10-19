#include "templates.h"
#include "blocks.h"
#include "deck.h"
#include "renderer.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace templates {

QString libraryDirectory() {
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return data.isEmpty() ? QString() : data + "/hype/templates";
}

namespace {

// The file a template folder is read from, as loadTemplate reads it.
QString fileIn(const QString &folder) {
    for (const char *name : {"template.md", "presentation.md"})
        if (QFileInfo(folder + "/" + name).isFile()) return folder + "/" + name;
    return {};
}

QString readText(const QString &path) {
    if (path.isEmpty()) return {};
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

Entry describe(const QString &name, const QString &path, const QString &where) {
    Entry entry;
    entry.name = name;
    entry.path = path;
    entry.where = where;
    const QFileInfo info(path);
    if (where == "library" && !path.startsWith(":/") && info.canonicalFilePath() != info.absoluteFilePath())
        entry.linked = info.canonicalFilePath();
    const QString text = readText(path);
    const ParsedDeck parsed = parseDeck(text);
    entry.title = scalar(parsed.header, "title", name);
    entry.theme = scalar(parsed.header, "theme");
    entry.buildsOn = scalar(parsed.header, "template");
    static const QRegularExpression defined("<Define\\s+name\\s*=\\s*\"([A-Z][A-Za-z0-9]*)\"");
    for (auto it = defined.globalMatch(definitionsIn(text.mid(parsed.header.size()))); it.hasNext();) {
        const QString component = it.next().captured(1);
        if (!entry.components.contains(component)) entry.components << component;
    }
    for (const Starter &starter : starterSlides(path)) {
        entry.starters << starter.label;
        entry.sections << starter.section;
    }
    entry.slides = int(entry.starters.size());
    return entry;
}

// Drops the lines of the slide's <Define> blocks, the comment that documents each one,
// and a blank line a block leaves doubled.
QString withoutDefinitionLines(QString source) {
    if (!source.contains("<Define")) return source;
    static const QRegularExpression documented(
        "<!--(?!\\s*hype:)(?:(?!-->)[\\s\\S])*-->[ \\t]*\\n(?:[ \\t]*\\n)*(?=[ \\t]*<Define\\b)");
    source.remove(documented);
    const QStringList lines = source.split('\n'), shown = withoutDefinitions(source).split('\n');
    QStringList kept;
    bool dropped = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (shown.value(i).trimmed().isEmpty() && !lines[i].trimmed().isEmpty()) {
            dropped = true;
            continue;
        }
        if (dropped && lines[i].trimmed().isEmpty() && (kept.isEmpty() || kept.last().trimmed().isEmpty())) continue;
        dropped = false;
        kept << lines[i];
    }
    return kept.join('\n');
}

void copyFolder(const QString &from, const QString &to) {
    if (!QFileInfo(from).isDir()) return;
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString target = to + "/" + QDir(from).relativeFilePath(file);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::copy(file, target);
    }
}

// Removes a library entry; links inside it go, never what they point at.
QString removeEntry(const QString &target) {
    const QFileInfo info(target);
    if (info.isSymLink()) return QFile::remove(target) ? QString() : "Could not remove " + target;
    return QDir(target).removeRecursively() ? QString() : "Could not remove " + target;
}

} // namespace

QList<Entry> available(const QString &base) {
    QList<Entry> found;
    QStringList names;
    auto scan = [&](const QString &root, const QString &where) {
        if (root.isEmpty()) return;
        for (const QString &folder : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
            const QString path = fileIn(root + "/" + folder);
            if (path.isEmpty() || names.contains(folder)) continue;
            names << folder;
            found << describe(folder, path, where);
        }
    };
    scan(QDir(base).absoluteFilePath("templates"), "project");
    scan(libraryDirectory(), "library");
    scan(":/templates", "built-in");
    return found;
}

QString withoutTemplateMarks(const QString &source) {
    QString shown = withoutDefinitionLines(source);
    if (!shown.contains("section=")) return shown;
    // The section mark goes, and with it a hype comment left saying nothing.
    static const QRegularExpression mark("(<!--\\s*hype:[^>]*?)\\s*\\bsection=\"[^\"]*\"");
    static const QRegularExpression empty("^[ \\t]*<!--\\s*hype:\\s*-->[ \\t]*\\n?", QRegularExpression::MultilineOption);
    shown.replace(mark, "\\1");
    shown.remove(empty);
    return shown;
}

QList<Starter> starterSlides(const QString &path) {
    QList<Starter> starters;
    if (path.isEmpty() || QFileInfo(path).fileName() == "template.md") return starters;
    static const QRegularExpression comment("<!--[\\s\\S]*?-->");
    QString section;
    for (const Slide &slide : parseDeck(readText(path)).slides) {
        // A section starts on the slide that names it, even one that only holds definitions;
        // section="" ends one.
        static const QRegularExpression mark("<!--\\s*hype:[^>]*?\\bsection=\"([^\"]*)\"");
        if (const auto named = mark.match(outsideCode(slide.source)); named.hasMatch()) section = named.captured(1);
        const QString shown = withoutTemplateMarks(slide.source);
        if (QString(shown).remove(comment).trimmed().isEmpty()) continue;
        starters << Starter{shown, slideLabel(shown), section};
    }
    return starters;
}

QString slideLabel(const QString &source) {
    static const QRegularExpression comment("<!--[\\s\\S]*?-->"), tag("^\\s*<([A-Z][A-Za-z0-9]*)([^>]*)>",
        QRegularExpression::MultilineOption), tags("<[^>]*>"), heading("^#{1,6}\\s+(.+)$"),
        item("^([-*+]|\\d+\\.)\\s+"), marks("[*_`\\\\]|\\{[^}]*\\}");
    const QString text = QString(source).remove(comment);
    // The first component of the template's own, and what it calls itself.
    QString component, named;
    for (auto it = tag.globalMatch(text); it.hasNext() && component.isEmpty();) {
        const auto m = it.next();
        if (isBuiltInComponent(m.captured(1)) || m.captured(1) == "Define" || m.captured(1) == "Slot") continue;
        component = m.captured(1);
        for (const char *key : {"name", "title", "client"})
            if (const auto value = QRegularExpression(QString("\\b%1=\"([^\"]*)\"").arg(key)).match(m.captured(2));
                named.isEmpty() && value.hasMatch())
                named = value.captured(1);
    }
    // A heading names a slide best, then the component's title, then the first words.
    QString title, words;
    for (const QString &raw : QString(text).remove(tags).split('\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith("```") || line.startsWith("~~~") || line.startsWith('|')) continue;
        if (const auto m = heading.match(line); m.hasMatch()) {
            title = m.captured(1);
            break;
        }
        if (words.isEmpty()) words = QString(line).remove(item);
    }
    QString label = !title.isEmpty() ? title : !named.isEmpty() ? named : words;
    label = label.remove(marks).simplified();
    if (label.size() > 60) label = label.left(57).trimmed() + "…";
    return component.isEmpty() ? label : label.isEmpty() ? component : component + " · " + label;
}

QString suggestedName(const QString &presentation) {
    const QFileInfo info(presentation);
    QString name = info.fileName() == "presentation.md" || info.fileName() == "template.md"
                       ? info.absoluteDir().dirName() : info.completeBaseName();
    name = name.toLower().replace(QRegularExpression("[^a-z0-9._-]+"), "-");
    while (name.startsWith('-') || name.startsWith('.')) name.remove(0, 1);
    while (name.endsWith('-')) name.chop(1);
    return name.isEmpty() ? "template" : name;
}

QString save(const QString &presentation, const QString &requested, bool copy, bool replace, QString *saved) {
    const QFileInfo source(presentation);
    if (!source.isFile()) return presentation + " does not exist; save the presentation first";
    const QString name = requested.trimmed().isEmpty() ? suggestedName(presentation) : requested.trimmed();
    static const QRegularExpression valid("^[A-Za-z0-9][A-Za-z0-9._-]*$");
    if (!valid.match(name).hasMatch())
        return "Name the template with letters, digits, dots, dashes, or underscores, such as acme";
    const QString library = libraryDirectory();
    if (library.isEmpty()) return "There is no folder for the template library";
    const QString target = library + "/" + name;
    const QFileInfo existing(target);
    if (existing.exists() || existing.isSymLink()) {
        // A copy saved over itself would delete what it is copied from.
        if (source.canonicalFilePath().startsWith(existing.canonicalFilePath() + "/"))
            return "That presentation is the template " + name + " already";
        if (!replace) return "A template named " + name + " already exists; replace it or choose another name";
        if (const QString error = removeEntry(target); !error.isEmpty()) return error;
    }
    if (!QDir().mkpath(target)) return "Could not create " + target;
    const QString file = target + "/presentation.md";
    if (!copy) {
        if (!QFile::link(source.absoluteFilePath(), file)) {
            QDir(target).removeRecursively();
            return "Could not link " + file + " to " + source.absoluteFilePath();
        }
    } else {
        QString text = readText(source.absoluteFilePath());
        const ParsedDeck parsed = parseDeck(text);
        // A template found beside the presentation is named by its path from the copy;
        // names the library or Hype itself provides stay names.
        const QString named = scalar(parsed.header, "template");
        if (!named.isEmpty()) {
            QString found;
            for (const QString &candidate : templateCandidates(named, source.absolutePath()))
                if (found.isEmpty() && QFileInfo(candidate).isFile()) found = candidate;
            if (!found.isEmpty() && !found.startsWith(library + "/") && !found.startsWith(":/"))
                text = setScalar(parsed.header, "template", QFileInfo(found).absoluteFilePath()) +
                       text.mid(parsed.header.size());
        }
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(text.toUtf8()) < 0 || !out.commit()) {
            QDir(target).removeRecursively();
            return "Could not write " + file;
        }
        copyFolder(source.absolutePath() + "/images", target + "/images");
        copyFolder(source.absolutePath() + "/fonts", target + "/fonts");
    }
    // A template that would build on itself is no use to anyone.
    const TemplatePack pack = loadTemplate(name, library);
    if (pack.error.contains("builds on itself")) {
        QDir(target).removeRecursively();
        return "The template " + name + " would build on itself: " + pack.error;
    }
    if (saved) *saved = target;
    return {};
}

QString remove(const QString &name, QString *removed) {
    const QString library = libraryDirectory();
    const QString target = library + "/" + name;
    const QFileInfo info(target);
    if (name.isEmpty() || name.contains('/') || name.startsWith('.') || (!info.exists() && !info.isSymLink()))
        return "No template named " + name + " in " + library;
    const QString path = fileIn(target);
    const bool link = info.isSymLink() || (!path.isEmpty() && QFileInfo(path).isSymLink());
    const QString linked = link ? QFileInfo(path.isEmpty() ? target : path).canonicalFilePath() : QString();
    if (const QString error = removeEntry(target); !error.isEmpty()) return error;
    if (removed)
        *removed = linked.isEmpty() ? "Removed the template " + name
                                    : "Removed the template " + name + "; " + linked + " is untouched";
    return {};
}

} // namespace templates
