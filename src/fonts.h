#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

// Fonts a presentation names but the machine lacks come from Google Fonts. Every family
// there is openly licensed; Hype keeps one copy per user, with its license, in
// ~/.local/share/hype/fonts/<Family>/ and loads it for any presentation that asks.
namespace fonts {

// Font files in a folder (fonts/ beside a presentation or template, or a downloaded family).
// Call from the GUI thread.
void loadDirectory(const QString &directory);
// Changes whenever new font files load, so caches keyed on it lay text out again.
int epoch();

// The family to ask Qt for: some font files name each weight as its own family ("Poppins
// Black"), so a family at that weight is found under that name.
QString styled(const QString &family, int weight, bool italic = false);

// sans-serif, serif, monospace and the like: always available through fontconfig.
bool isGeneric(const QString &family);
// The family is installed, or loaded from a fonts/ folder or a download.
bool available(const QString &family);

// Families named in front matter (font, font_heading, font_<name>) and in `font="…"`
// props, leaving out the names that refer to those: heading, body and the font_ aliases.
QStringList requested(const QStringList &headers, const QStringList &texts);
// Loads what was downloaded for these families before.
void loadDownloaded(const QStringList &families);

QString downloadsDirectory();
QString downloadedDirectory(const QString &family);

struct Family {
    QString name, category;
    QStringList styles; // "400", "700i", …
};
// The Google Fonts catalog: cached for a week, fetched when `fetch` allows. Empty offline.
QVector<Family> catalog(bool fetch = true);
QStringList search(const QString &query, int limit = 40);
// The closest catalog names, for a misspelled family.
QStringList suggestions(const QString &family);

// Whether Hype may download fonts: HYPE_FONT_DOWNLOADS=0 turns it off.
bool downloadsAllowed();
// Downloads a Google Fonts family into the downloads folder. Blocking, from any thread;
// returns an error, or nothing on success or when it was already there.
QString download(const QString &family, QString *canonical = nullptr);

} // namespace fonts
