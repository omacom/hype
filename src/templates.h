#pragma once
#include <QList>
#include <QString>
#include <QStringList>

// The templates a presentation can build on: folders beside it in templates/, the
// library in ~/.local/share/hype/templates/, and the ones Hype comes with. Any
// presentation can join the library, as a link that follows its file or as a copy.
namespace templates {

struct Entry {
    QString name;  // What `template:` names it by.
    QString path;  // The file it is read from.
    QString where; // "project", "library" or "built-in".
    QString linked; // The presentation a library link follows.
    QString title, theme, buildsOn;
    QStringList components;
    QStringList starters; // Labels of the slides a new slide can start from.
    QStringList sections; // The section of each of those slides.
    int slides = 0;
};

// A template slide, without its definitions, ready to add to a presentation. A template
// groups its slides with <!-- hype: section="Case studies" --> on the first of each group.
struct Starter {
    QString source, label, section;
};

QString libraryDirectory();
// Every template a presentation in `base` can name, in the order names are looked up;
// a name appears once, as the one `template:` finds.
QList<Entry> available(const QString &base);
// The slides of the template at `path` that show something; a template.md has none.
QList<Starter> starterSlides(const QString &path);
// A template's slides as a copy reads: without its definitions or section marks.
QString withoutTemplateMarks(const QString &source);
// A short name for a slide: the component it is built on and its first words.
QString slideLabel(const QString &source);
// A library name for a presentation: its file's name, or its folder's for presentation.md.
QString suggestedName(const QString &presentation);
// Adds a presentation to the library under `name`. A link follows the file; a copy
// keeps it, its images and its fonts as they are now. Returns an error, or sets `saved`.
QString save(const QString &presentation, const QString &name, bool copy, bool replace, QString *saved);
// Removes a library template: a link leaves the presentation it follows alone.
QString remove(const QString &name, QString *removed);

} // namespace templates
