#pragma once
#include <QJsonObject>
#include <QMap>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantMap>
class QPainter;

// Components: MDX-style tags that compose a slide from layout containers and
// primitives. See plans/components.md and the guide printed by `hype help components`.

// True when the slide uses a component tag outside code and comments.
bool hasComponents(const QString &source);
// True when the slide is drawn by the component engine: it uses components, or a
// template frame applies to its plain Markdown.
bool isComposedSlide(const QString &source, const QVariantMap &palette);
// Errors: unknown components or props, unclosed tags, missing images or icons.
QStringList composedProblems(const QString &source, const QString &base,
                             const QVariantMap &palette);
// What the slide draws without: components, props and slots of another template than the
// one in use. The slide still shows, so these are for telling the author, not for refusing.
QStringList composedWarnings(const QString &source, const QString &base,
                             const QVariantMap &palette);
// Paints in 1920×1080 slide units; the caller has already scaled the painter.
// `warning` receives fit and readability warnings.
// With `overVideo`, only what lies over the slide's video is painted, on a clear ground,
// for a player to show beneath it.
void paintComposed(QPainter *painter, const QString &source, const QString &base,
                   const QVariantMap &palette, QString *warning, QStringList *problems = nullptr,
                   bool overVideo = false);
// The first <Video> written on the slide: its props, and where the layout puts it.
QMap<QString, QString> componentVideo(const QString &source);
// The <Video> a template's component brings to the slide, which the slide's own text does
// not show: found by laying the slide out.
QMap<QString, QString> definedVideo(const QString &source, const QString &base,
                                    const QVariantMap &palette);
QRectF componentMediaBox(const QString &source, const QString &base, const QVariantMap &palette);
// Every picture and video file a composed slide shows, so caches can notice when one changes.
QStringList componentAssets(const QString &source, const QString &base, const QVariantMap &palette);

// A template: any presentation, or a template.md, whose front matter sets the defaults of
// presentations built on it and whose <Define> blocks become their components. It may
// build on a template of its own, and so on.
struct TemplatePack {
    QString path;            // The template named, if found.
    QString header;          // Each template's front matter, the nearest first.
    QString definitions;     // Each template's definitions, the nearest last so they win.
    QString theme;           // The nearest theme a template names.
    QStringList directories; // Where each template lives, the nearest first.
    QStringList watched;     // Every file whose change or arrival would change the template.
    QString error;
};
// `presentation` is the file using the template, which may not be among its templates.
TemplatePack loadTemplate(const QString &name, const QString &base,
                          const QString &presentation = {});
// Where loadTemplate looks for a template, in order.
QStringList templateCandidates(const QString &name, const QString &base);
// The <Define> blocks written on a presentation's slides; every slide may use them.
QString definitionsIn(const QString &text);
// The slide as it shows: its <Define> blocks blanked, line for line.
QString withoutDefinitions(const QString &source);
// Registers definitions and returns the id palettes refer to them by, as template_id.
QString rememberTemplate(const QString &text);
// Registers what the templates in these files call their components, props and slots, and
// returns the id palettes refer to it by, as vocabulary_id.
QString rememberVocabulary(const QStringList &files);
// A vocabulary's words, to hand to another process with the palette that names it, and
// to take them up there under the same id.
QJsonObject vocabularyWords(const QString &id);
void restoreVocabulary(const QString &id, const QJsonObject &words);
// Whether the definitions and vocabulary a palette names are still held. Only recent ones
// are kept, so a palette cached for a long while checks before it is used again.
bool remembers(const QVariantMap &palette);
// Keeps what a palette names past the registries' limit, for as long as something draws
// with it; every hold is given back with a release.
void holdLook(const QVariantMap &palette);
void releaseLook(const QVariantMap &palette);
bool templateShowsPages(const QString &id);

// True for a component Hype itself provides, under any of its names.
bool isBuiltInComponent(const QString &name);
QString componentGuide();
// Icon names matching every word of the query, best first; all names for an empty query.
QStringList searchIcons(const QString &query, int limit = 40);
