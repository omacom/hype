#include "deck.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>
#include <chrono>

namespace {
QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool writeAtomically(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}

QString Deck::recoveryFolder() const {
    return m_recoveryDirectory + '/' + digest((m_path.isEmpty() ? QString("untitled") : m_path).toUtf8());
}
void Deck::enableAutosave(const QString &directory) {
    if (!m_recoveryDirectory.isEmpty()) return;
    m_recoveryDirectory = directory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::StateLocation) + "/recovery" : directory;
    m_autosaveTimer.setSingleShot(true);
    m_autosaveTimer.setInterval(1000);
    m_autosaveDeadline.setSingleShot(true);
    m_autosaveDeadline.setInterval(5000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &Deck::flushAutosave);
    connect(&m_autosaveDeadline, &QTimer::timeout, this, &Deck::flushAutosave);
    connect(this, &Deck::changed, this, [this] {
        if (m_recovering || (m_source == m_checkpointSource && m_path == m_checkpointPath)) return;
        m_autosaveTimer.start();
        if (!m_autosaveDeadline.isActive()) m_autosaveDeadline.start();
    });
    recoverDraft();
    checkpoint();
    if (dirty() && !m_path.isEmpty() && !m_externalChange) m_autosaveTimer.start();
}
bool Deck::checkpoint() {
    if (m_recoveryDirectory.isEmpty()) return false;
    const QString folder = recoveryFolder();
    QJsonArray slides;
    for (const auto &slide : m_parsed.slides)
        slides.append(QJsonObject{{"start", slide.start}, {"end", slide.end}});
    const QByteArray bytes = QJsonDocument(QJsonObject{
        {"version", 1}, {"path", m_path}, {"source", m_source}, {"saved", m_saved},
        {"sha256", digest(m_source.toUtf8())}, {"header", m_parsed.header}, {"slides", slides},
        {"selected", m_selected}, {"anchor", m_anchor}, {"conflict", m_externalChange},
        {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}).toJson();
    if (!QDir().mkpath(folder + "/versions")) {
        setStatus("Could not create the recovery folder. Changes are still in the editor.");
        return false;
    }
    // Keep every distinct checkpoint. These contain Markdown, never media copies.
    // Write the immutable version before replacing the latest crash-recovery file.
    if (m_source != m_checkpointSource || m_path != m_checkpointPath) {
        const auto order = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const QString version = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") +
            '-' + QString::number(order).rightJustified(20, '0') + '-' +
            digest(m_source.toUtf8()).left(16) + ".json";
        if (!writeAtomically(folder + "/versions/" + version, bytes)) {
            setStatus("Could not back up this version. Changes are still in the editor.");
            return false;
        }
    }
    if (!writeAtomically(folder + "/latest.json", bytes)) {
        setStatus("Could not save the recovery draft. Changes are still in the editor.");
        return false;
    }
    m_checkpointSource = m_source;
    m_checkpointPath = m_path;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "hype", "hype");
    settings.setValue("files/lastRecoveryDocument", m_path);
    settings.sync();
    return true;
}
bool Deck::flushAutosave() {
    m_autosaveTimer.stop();
    m_autosaveDeadline.stop();
    if (!checkpoint()) return false;
    if (dirty() && !m_path.isEmpty()) {
        const auto parsed = parseDeck(m_source);
        bool complete = parsed.error.isEmpty() && m_parsed.error.isEmpty() && parsed.slides.size() == count();
        for (int i = 0; complete && i < count(); ++i)
            complete = parsed.slides[i].source == m_parsed.slides[i].source;
        if (!complete) setStatus("Draft backed up · finish the Markdown to save the presentation");
        else if (!m_externalChange) {
            savePath(m_path);
        } else setStatus("Draft backed up · file changed outside Hype; use Save As to keep both");
    } else if (m_path.isEmpty()) setStatus("Draft backed up · Ctrl+S to choose a file");
    return true; // A recoverable draft is sufficient to close, even if the file cannot be saved.
}
bool Deck::restoreSnapshot(const QByteArray &bytes, bool opening) {
    const auto snapshot = QJsonDocument::fromJson(bytes).object();
    if (snapshot["version"].toInt() != 1 || !snapshot["source"].isString() ||
        snapshot["path"].toString() != m_path || !snapshot["saved"].isString()) return false;
    const QString source = snapshot["source"].toString();
    if (digest(source.toUtf8()) != snapshot["sha256"].toString()) return false;
    ParsedDeck parsed;
    parsed.header = snapshot["header"].toString();
    if (!source.startsWith(parsed.header)) return false;
    const auto slides = snapshot["slides"].toArray();
    int previousEnd = parsed.header.size();
    for (int i = 0; i < slides.size(); ++i) {
        const auto slide = slides[i].toObject();
        const int start = slide["start"].toInt(-1), end = slide["end"].toInt(-1);
        if (start < previousEnd || end < start || end > source.size()) return false;
        const QString separator = source.mid(previousEnd, start - previousEnd);
        const bool terminal = i == slides.size() - 1 && start == source.size() && start == end &&
                              (separator == "---" || separator == "---\r");
        if (i == 0 ? !separator.isEmpty() : !terminal && separator != "---\n" && separator != "---\r\n") return false;
        parsed.slides.append({source.mid(start, end - start), start, end});
        previousEnd = end;
    }
    if (parsed.slides.isEmpty() || previousEnd != source.size()) return false;
    // A clean checkpoint is history, not an unsaved draft. Respect a newer file
    // from Dropbox or another editor instead of resurrecting the old contents.
    if (opening && source == snapshot["saved"].toString() && source != m_saved && !m_saved.isEmpty())
        return true;
    const bool conflict = opening && source != m_saved &&
        (snapshot["conflict"].toBool() || snapshot["saved"].toString() != m_saved);
    m_recovering = true;
    apply(source, snapshot["selected"].toInt(), !opening, snapshot["anchor"].toInt(), &parsed);
    m_recovering = false;
    if (conflict) setExternalChange(true);
    m_checkpointSource = source;
    m_checkpointPath = m_path;
    if (opening && dirty())
        setStatus(conflict ? "Recovered draft · file also changed outside Hype; use Save As to keep both"
                           : "Recovered your last draft");
    if (!opening) setStatus("Restored earlier version");
    return true;
}
void Deck::recoverDraft() {
    if (m_recoveryDirectory.isEmpty()) return;
    m_autosaveTimer.stop();
    m_autosaveDeadline.stop();
    m_checkpointSource.clear();
    m_checkpointPath.clear();
    QFile latest(recoveryFolder() + "/latest.json");
    if (latest.exists() && latest.open(QIODevice::ReadOnly)) {
        const auto bytes = latest.readAll();
        if (QJsonDocument::fromJson(bytes).object()["retired"].toBool()) return;
        if (restoreSnapshot(bytes, true)) return;
    }
    // A crash can occur after writing the immutable version but before publishing latest.json.
    for (const auto &entry : recoveryVersions()) {
        QFile version(recoveryFolder() + "/versions/" + entry.toMap()["name"].toString());
        if (version.open(QIODevice::ReadOnly) && restoreSnapshot(version.readAll(), true)) {
            setStatus("Recovered from version history; the latest recovery file was missing or damaged");
            return;
        }
    }
    if (latest.exists())
        setStatus("Could not read the recovery draft. Earlier versions are available in History.");
}
void Deck::retireDraft() {
    if (m_recoveryDirectory.isEmpty()) return;
    m_autosaveTimer.stop();
    m_autosaveDeadline.stop();
    // Save As retires the old active draft, but leaves its history available.
    // A marker distinguishes intentional retirement from a missing file after a crash.
    writeAtomically(recoveryFolder() + "/latest.json", "{\"retired\":true}\n");
}
QVariantList Deck::recoveryVersions() const {
    QVariantList result;
    if (m_recoveryDirectory.isEmpty()) return result;
    const QDir versions(recoveryFolder() + "/versions");
    for (const auto &name : versions.entryList({"*.json"}, QDir::Files, QDir::Name | QDir::Reversed)) {
        // Metadata comes from the filename, so opening History doesn't read every document.
        const auto time = QDateTime::fromString(name.left(19), "yyyyMMdd-HHmmss-zzz");
        auto utc = time;
        utc.setTimeZone(QTimeZone::UTC);
        result.append(QVariantMap{{"name", name}, {"label", utc.toLocalTime().toString("yyyy-MM-dd HH:mm:ss.zzz")}});
    }
    return result;
}
bool Deck::restoreVersion(const QString &name) {
    if (m_recoveryDirectory.isEmpty() || name != QFileInfo(name).fileName() || !name.endsWith(".json")) return false;
    QFile version(recoveryFolder() + "/versions/" + name);
    if (!version.open(QIODevice::ReadOnly)) { setStatus("Could not read that version."); return false; }
    if (!checkpoint()) return false;
    if (!restoreSnapshot(version.readAll(), false)) { setStatus("That recovery version is damaged."); return false; }
    // Capture the restoration as a new version; preserve the version being restored.
    m_checkpointSource.clear();
    return flushAutosave();
}
