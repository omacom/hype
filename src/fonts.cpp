#include "fonts.h"
#include "deck.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <atomic>

namespace fonts {
namespace {

std::atomic_int loadedEpoch{0};

QString compact(QString text) { return text.toLower().remove(QRegularExpression("[^a-z0-9]")); }

// Google's endpoints, or a stand-in server for tests (HYPE_FONTS_SERVER).
QString server(const QString &google) {
    const QString local = qEnvironmentVariable("HYPE_FONTS_SERVER");
    if (local.isEmpty()) return google;
    const QUrl url(google);
    return local + url.path() + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded) : QString());
}

QByteArray get(const QString &address, QString *error) {
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(address)};
    // Without a browser's user agent, the CSS API answers with TrueType files.
    request.setHeader(QNetworkRequest::UserAgentHeader, "hype");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray data;
    if (reply->error() == QNetworkReply::NoError) data = reply->readAll();
    else if (error) *error = reply->errorString();
    reply->deleteLater();
    return data;
}

// Google prefixes some JSON with )]}' so a browser can't run it as script.
QJsonDocument json(QByteArray data) {
    if (data.startsWith(")]}'")) data = data.mid(4);
    return QJsonDocument::fromJson(data);
}

int editDistance(const QString &a, const QString &b) {
    QVector<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int previous = row[0];
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int current = row[j];
            row[j] = qMin(qMin(row[j] + 1, row[j - 1] + 1), previous + (a[i - 1] == b[j - 1] ? 0 : 1));
            previous = current;
        }
    }
    return row[b.size()];
}

struct Catalog {
    QMutex mutex;
    QVector<Family> families;
    QHash<QString, int> index; // Compact name to position.
    bool loaded = false, fetched = false;
    QString path; // The cache file it was loaded from.
};
Catalog &shared() {
    static Catalog catalog;
    return catalog;
}
QString catalogFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/hype/google-fonts.json";
}
// The cache keeps only what Hype uses, about a tenth of Google's metadata.
void parseCatalog(const QJsonArray &list, Catalog &catalog) {
    catalog.families.clear();
    catalog.index.clear();
    for (const QJsonValue &value : list) {
        const QJsonObject object = value.toObject();
        Family family;
        family.name = object["name"].toString();
        family.category = object["category"].toString();
        for (const QJsonValue &style : object["styles"].toArray()) family.styles << style.toString();
        if (family.name.isEmpty()) continue;
        catalog.index.insert(compact(family.name), catalog.families.size());
        catalog.families << family;
    }
}
QString weightName(int weight) {
    static const QHash<int, QString> names{{100, "Thin"},   {200, "ExtraLight"}, {300, "Light"},
                                           {400, "Regular"}, {500, "Medium"},    {600, "SemiBold"},
                                           {700, "Bold"},   {800, "ExtraBold"},  {900, "Black"}};
    return names.value(weight, QString::number(weight));
}

} // namespace

void loadDirectory(const QString &directory) {
    static QMutex mutex;
    static QSet<QString> loaded;
    QDir dir(directory);
    if (!dir.exists()) return;
    QMutexLocker lock(&mutex);
    bool added = false;
    for (const QFileInfo &file : dir.entryInfoList({"*.ttf", "*.otf", "*.TTF", "*.OTF"}, QDir::Files)) {
        const QString key = file.absoluteFilePath() + ':' + QString::number(file.lastModified().toMSecsSinceEpoch());
        if (loaded.contains(key)) continue;
        loaded.insert(key);
        added = QFontDatabase::addApplicationFont(file.absoluteFilePath()) >= 0 || added;
    }
    if (added) ++loadedEpoch;
}

int epoch() { return loadedEpoch.load(); }

QString styled(const QString &family, int weight, bool italic) {
    Q_UNUSED(italic); // A legacy-named weight keeps its italic as a style of that family.
    if (weight == 400 || weight == 700) return family; // Regular and bold are always the family's own.
    static QMutex mutex;
    static QSet<QString> families;
    static int seen = -1;
    QMutexLocker lock(&mutex);
    if (seen != loadedEpoch.load()) {
        const QStringList all = QFontDatabase::families();
        families = QSet<QString>(all.begin(), all.end());
        seen = loadedEpoch.load();
    }
    const QString legacy = family + " " + weightName(qBound(100, (weight + 50) / 100 * 100, 900));
    return families.contains(legacy) ? legacy : family;
}

bool isGeneric(const QString &family) {
    static const QStringList generic{"sans",    "sans-serif", "serif", "monospace", "mono", "system-ui",
                                     "cursive", "fantasy",    "emoji", "math",      "ui-monospace"};
    return generic.contains(family.trimmed().toLower());
}

bool available(const QString &family) {
    if (family.trimmed().isEmpty() || isGeneric(family)) return true;
    const QString wanted = compact(family);
    for (const QString &installed : QFontDatabase::families()) {
        const QString name = compact(installed);
        // JetBrains Mono often arrives as JetBrainsMono Nerd Font, which fontconfig substitutes.
        if (name == wanted || (name.startsWith(wanted) && QRegularExpression("^(nerdfont|nf)(mono|propo|m|p)?$")
                                                              .match(name.mid(wanted.size()))
                                                              .hasMatch()))
            return true;
    }
    return false;
}

QStringList requested(const QStringList &headers, const QStringList &texts) {
    static const QRegularExpression key("^(font(?:_[a-z0-9_]+)?)\\s*:", QRegularExpression::MultilineOption);
    static const QRegularExpression prop("\\bfont\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')");
    QStringList names, aliases{"heading", "body", "title", "number", "eyebrow"};
    for (const QString &header : headers)
        for (auto it = key.globalMatch(header); it.hasNext();) {
            const QString name = it.next().captured(1);
            if (name.startsWith("font_")) aliases << name.mid(5);
            names << scalar(header, name);
        }
    for (const QString &text : texts)
        for (auto it = prop.globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            names << (m.capturedStart(1) >= 0 ? m.captured(1) : m.captured(2));
        }
    QStringList families;
    for (const QString &raw : names) {
        const QString name = raw.trimmed();
        // Template placeholders such as {font} are filled in later, and aliases name a family above.
        if (name.isEmpty() || name.contains('{') || aliases.contains(name, Qt::CaseInsensitive) || isGeneric(name))
            continue;
        if (!families.contains(name, Qt::CaseInsensitive)) families << name;
    }
    return families;
}

QString downloadsDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/hype/fonts";
}

QString downloadedDirectory(const QString &family) {
    QString folder = family.trimmed();
    folder.replace(QRegularExpression("[/\\\\:]"), "-");
    // Downloads are stored under the catalog's spelling; find them under any other.
    const QDir root(downloadsDirectory());
    for (const QString &existing : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (compact(existing) == compact(folder)) return root.filePath(existing);
    return root.filePath(folder);
}

void loadDownloaded(const QStringList &families) {
    for (const QString &family : families) {
        const QString directory = downloadedDirectory(family);
        if (QFileInfo::exists(directory)) loadDirectory(directory);
    }
}

bool downloadsAllowed() { return qEnvironmentVariable("HYPE_FONT_DOWNLOADS") != "0"; }

QVector<Family> catalog(bool fetch) {
    Catalog &catalog = shared();
    QMutexLocker lock(&catalog.mutex);
    const QFileInfo cache(catalogFile());
    if (catalog.path != cache.absoluteFilePath()) {
        catalog.families.clear();
        catalog.index.clear();
        catalog.loaded = catalog.fetched = false;
        catalog.path = cache.absoluteFilePath();
    }
    const bool fresh = cache.exists() && cache.lastModified().daysTo(QDateTime::currentDateTime()) < 7;
    if (!catalog.loaded && cache.exists()) {
        QFile file(cache.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly)) parseCatalog(QJsonDocument::fromJson(file.readAll()).array(), catalog);
        catalog.loaded = true;
    }
    if (fetch && !fresh && !catalog.fetched && downloadsAllowed()) {
        catalog.fetched = true; // Once a process, whether or not it works.
        const QJsonDocument metadata = json(get(server("https://fonts.google.com/metadata/fonts"), nullptr));
        const QJsonArray list = metadata.object()["familyMetadataList"].toArray();
        if (!list.isEmpty()) {
            QVector<QPair<int, QJsonObject>> ranked;
            for (const QJsonValue &value : list) {
                const QJsonObject object = value.toObject();
                QJsonArray styles;
                for (const QString &style : object["fonts"].toObject().keys()) styles << style;
                ranked.append({object["popularity"].toInt(1 << 20),
                               QJsonObject{{"name", object["family"]}, {"category", object["category"]}, {"styles", styles}}});
            }
            // Most popular first, so searches lead with the fonts people reach for.
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            QJsonArray compactList;
            for (const auto &entry : ranked) compactList << entry.second;
            QDir().mkpath(cache.absolutePath());
            QSaveFile file(cache.absoluteFilePath());
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(compactList).toJson(QJsonDocument::Compact));
                file.commit();
            }
            parseCatalog(compactList, catalog);
            catalog.loaded = true;
        }
    }
    return catalog.families;
}

QStringList search(const QString &query, int limit) {
    const QVector<Family> list = catalog();
    const QString wanted = query.trimmed().toLower();
    QVector<QPair<int, QString>> scored;
    for (int i = 0; i < list.size(); ++i) {
        const QString name = list[i].name.toLower();
        int score = -1;
        if (wanted.isEmpty() || name == wanted) score = 0;
        else if (name.startsWith(wanted)) score = 1;
        else if (name.contains(wanted)) score = 2;
        else if (list[i].category.toLower() == wanted) score = 3;
        if (score >= 0) scored.append({score * 100000 + i, list[i].name});
    }
    std::sort(scored.begin(), scored.end());
    QStringList names;
    for (const auto &entry : scored) {
        if (limit > 0 && names.size() >= limit) break;
        names << entry.second;
    }
    return names;
}

QStringList suggestions(const QString &family) {
    const QVector<Family> list = catalog(false);
    const QString wanted = compact(family);
    QVector<QPair<int, QString>> near;
    for (int i = 0; i < list.size(); ++i) {
        const int distance = editDistance(wanted, compact(list[i].name));
        if (distance <= qMax(2, int(wanted.size()) / 4)) near.append({distance * 10000 + i, list[i].name});
    }
    std::sort(near.begin(), near.end());
    QStringList names;
    for (int i = 0; i < near.size() && names.size() < 3; ++i) names << near[i].second;
    return names;
}

QString download(const QString &family, QString *canonical) {
    static QMutex mutex; // One download at a time; a second request for a family finds it done.
    QMutexLocker lock(&mutex);
    if (!downloadsAllowed()) return "font downloads are turned off (HYPE_FONT_DOWNLOADS=0)";
    QString name = family.trimmed();
    const QVector<Family> list = catalog();
    QStringList styles{"400"};
    const auto entry = std::find_if(list.begin(), list.end(), [&](const Family &f) { return compact(f.name) == compact(name); });
    if (entry != list.end()) {
        name = entry->name;
        styles = entry->styles;
    } else if (!list.isEmpty()) {
        // Without the catalog (offline), the CSS API itself says whether the family exists.
        const QStringList near = suggestions(family);
        return "“" + family + "” is not installed and not on Google Fonts" +
               (near.isEmpty() ? QString() : "; did you mean " + near.join(", ") + "?");
    }
    if (canonical) *canonical = name;
    const QString target = downloadedDirectory(name);
    if (!QDir(target).entryList({"*.ttf", "*.otf"}, QDir::Files).isEmpty()) return {};
    QDir().mkpath(downloadsDirectory());
    QTemporaryDir staging(downloadsDirectory() + "/.download-XXXXXX");
    if (!staging.isValid()) return "could not write to " + downloadsDirectory();
    QString error;
    auto save = [&](const QString &file, const QString &url) {
        const QByteArray data = get(server(url), &error);
        const bool font = data.startsWith(QByteArray("\x00\x01\x00\x00", 4)) || data.startsWith("OTTO") ||
                          data.startsWith("true");
        if (!font) return false;
        QSaveFile out(staging.filePath(file));
        return out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit();
    };

    // The family's download, as the website's Download button gives it: static files whose
    // names say both "Poppins" and "Black", so every weight is found under the one family.
    const QJsonObject manifest =
        json(get(server("https://fonts.google.com/download/list?family=" + QUrl::toPercentEncoding(name)), nullptr))
            .object()["manifest"]
            .toObject();
    QVector<QPair<QString, QString>> files; // name, url
    const QString prefix = QString(name).remove(' ') + "-";
    for (const QJsonValue &value : manifest["fileRefs"].toArray()) {
        const QString file = QFileInfo(value.toObject()["filename"].toString()).fileName();
        // Wider and narrower cuts of a family (EncodeSans_Condensed-…) are families of their own.
        if (file.startsWith(prefix, Qt::CaseInsensitive) && !file.contains("VariableFont") &&
            (file.endsWith(".ttf") || file.endsWith(".otf")))
            files.append({file, value.toObject()["url"].toString()});
    }
    int saved = 0;
    for (const auto &[file, url] : files) {
        if (!save(file, url)) return "could not download " + file + (error.isEmpty() ? "" : ": " + error);
        ++saved;
    }

    if (!saved) {
        // Otherwise ask the CSS API for every style the family has, one file per style.
        QVector<QPair<int, int>> tuples; // italic, weight
        bool italics = false;
        for (const QString &style : styles) {
            const bool italic = style.endsWith('i');
            const int weight = QString(style).remove('i').toInt();
            if (weight <= 0) continue;
            italics = italics || italic;
            tuples.append({italic, weight});
        }
        std::sort(tuples.begin(), tuples.end());
        QStringList axes;
        for (const auto &[italic, weight] : tuples)
            axes << (italics ? QString("%1,%2").arg(italic).arg(weight) : QString::number(weight));
        QString spec = QString::fromLatin1(QUrl::toPercentEncoding(name));
        if (!(tuples.size() == 1 && tuples[0] == qMakePair(0, 400)) && !tuples.isEmpty())
            spec += (italics ? ":ital,wght@" : ":wght@") + axes.join(';');
        const QString css = QString::fromUtf8(get(server("https://fonts.googleapis.com/css2?family=" + spec), &error));
        if (css.isEmpty())
            return "could not reach Google Fonts for “" + name + "”" + (error.isEmpty() ? "" : ": " + error);
        static const QRegularExpression face("@font-face\\s*\\{([^}]*)\\}");
        static const QRegularExpression styleRe("font-style:\\s*(\\w+)"), weightRe("font-weight:\\s*(\\d+)"),
            srcRe("src:\\s*url\\(([^)]+)\\)");
        QSet<QString> seen;
        for (auto it = face.globalMatch(css); it.hasNext();) {
            const QString block = it.next().captured(1);
            const QString url = srcRe.match(block).captured(1).remove('\'').remove('"');
            const int weight = weightRe.match(block).captured(1).toInt();
            const bool italic = styleRe.match(block).captured(1) == "italic";
            if (url.isEmpty() || weight <= 0) continue;
            const QString file = prefix + (weight == 400 && italic ? QString() : weightName(weight)) +
                                 (italic ? "Italic" : "") + ".ttf";
            if (seen.contains(file)) continue; // A family split into subsets lists a style more than once.
            seen.insert(file);
            if (!save(file, url)) return "could not download " + file + (error.isEmpty() ? "" : ": " + error);
            ++saved;
        }
    }
    if (!saved) return "Google Fonts sent no font files for “" + name + "”";
    // The license travels with the files.
    for (const QJsonValue &value : manifest["files"].toArray()) {
        const QJsonObject entry = value.toObject();
        const QString file = QFileInfo(entry["filename"].toString()).fileName();
        if (!file.endsWith(".txt") || file.startsWith("README")) continue;
        QSaveFile out(staging.filePath(file));
        if (out.open(QIODevice::WriteOnly)) {
            out.write(entry["contents"].toString().toUtf8());
            out.commit();
        }
    }
    QDir(target).removeRecursively();
    if (!QDir().rename(staging.path(), target)) return "could not move the download into " + target;
    staging.setAutoRemove(false);
    return {};
}

} // namespace fonts
