#include "apptheme.h"
#include "deck.h"
#include "filedialog.h"
#include "renderer.h"
#include "images.h"
#include "syntax.h"
#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QFile>
#include <QJsonDocument>
#include <QJSEngine>
#include <QJsonObject>
#include <QImage>
#include <QImageReader>
#include <QGlyphRun>
#include <QMimeData>
#include <QMediaPlayer>
#include <QPainter>
#include <QPdfDocument>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItemGrabResult>
#include <QSettings>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextLayout>
#include <QVideoSink>
#include <QVideoFrame>
#include <QtTest>

class TestFilePortal : public QDBusVirtualObject {
  public:
    QString method, title, selected, alternatePath;
    QVariantMap options;
    uint result = 0;
    bool respondBeforeReply = false;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (message.interface() != "org.freedesktop.portal.FileChooser") return false;
        method = message.member();
        title = message.arguments().at(1).toString();
        options = qdbus_cast<QVariantMap>(message.arguments().at(2));
        QString sender = message.service().mid(1);
        sender.replace('.', '_');
        const QString path = alternatePath.isEmpty()
            ? "/org/freedesktop/portal/desktop/request/" + sender + "/" + options["handle_token"].toString()
            : alternatePath;
        auto reply = [=] { bus.send(message.createReply(QVariant::fromValue(QDBusObjectPath(path)))); };
        auto response = [=] {
            auto signal = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
            signal.setArguments({result, QVariantMap{{"uris", QStringList{selected}}}});
            bus.send(signal);
        };
        if (respondBeforeReply) {
            response();
            QTimer::singleShot(10, this, reply);
        } else {
            reply();
            QTimer::singleShot(10, this, response);
        }
        return true;
    }
};

class HypeTests : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDirectory;
    static void write(const QString &path, const QString &content) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content.toUtf8());
    }
  private slots:
    void initTestCase() {
        QVERIFY(settingsDirectory.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    }
    void portalFileDialogs() {
        if (!qEnvironmentVariableIsSet("HYPE_PORTAL_TESTS"))
            QSKIP("Run with HYPE_PORTAL_TESTS=1 under dbus-run-session");
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "test-file-portal");
        QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
        TestFilePortal portal;
        QVERIFY(bus.registerVirtualObject("/org/freedesktop/portal/desktop", &portal));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterObject("/org/freedesktop/portal/desktop");
            bus.unregisterService("org.freedesktop.portal.Desktop");
        });
        QString error;
        const QString selected = "/tmp/Slides with spaces/æ.md";
        portal.selected = QUrl::fromLocalFile(selected).toString();
        QCOMPARE(FileDialog::choose(false, "/tmp", "Markdown", {"*.md"}, &error), selected);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(portal.method, "OpenFile");
        QCOMPARE(portal.title, "Open File");
        QCOMPARE(portal.options["current_folder"].toByteArray(), QByteArray("/tmp\0", 5));
        QCOMPARE(portal.options["modal"].toBool(), true);
        const QDBusArgument filters = portal.options["filters"].value<QDBusArgument>();
        QCOMPARE(filters.currentSignature(), "a(sa(us))");
        QString label, pattern;
        uint type;
        filters.beginArray(); filters.beginStructure(); filters >> label;
        filters.beginArray(); filters.beginStructure(); filters >> type >> pattern;
        filters.endStructure(); filters.endArray(); filters.endStructure(); filters.endArray();
        QCOMPARE(label, "Markdown"); QCOMPARE(type, 0u); QCOMPARE(pattern, "*.md");

        portal.alternatePath = "/org/freedesktop/portal/desktop/request/legacy";
        QCOMPARE(FileDialog::choose(true, selected, "Markdown", {"*.md"}, &error), selected);
        QCOMPARE(portal.method, "SaveFile");
        QCOMPARE(portal.title, "Save File");
        QCOMPARE(portal.options["current_name"].toString(), "æ.md");
        QVERIFY2(error.isEmpty(), qPrintable(error));

        portal.alternatePath.clear();
        portal.respondBeforeReply = true;
        portal.result = 1;
        QVERIFY(FileDialog::choose(false, "/tmp", "Media", {"*.png", "*.mp4"}, &error).isEmpty());
        QVERIFY(error.isEmpty()); // Cancellation is not an error.
        portal.result = 0;
        portal.selected = "https://example.org/image.png";
        QVERIFY(FileDialog::choose(false, "/tmp", "Media", {"*.png"}, &error).isEmpty());
        QCOMPARE(error, "Choose a local file.");
        portal.result = 2;
        QVERIFY(FileDialog::choose(false, "/tmp", "Media", {"*.png"}, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void followsDesktopTheme() {
        QTemporaryDir files;
        const QString current = files.path() + "/current";
        const QString dark = files.path() + "/dark";
        const QString light = files.path() + "/light";
        QVERIFY(QDir().mkpath(current));
        QVERIFY(QDir().mkpath(dark));
        QVERIFY(QDir().mkpath(light));
        write(dark + "/colors.toml", "background = '#101020'\nforeground = '#eeeeee'\naccent = '#7788ff'\n");
        write(light + "/colors.toml", "background = '#ffffff'\nforeground = '#111111'\naccent = '#224488'\n");
        write(light + "/hyprland.lua", "hl.config({\n  decoration = {\n    rounding = 6,\n    -- rounding = 12,\n  },\n})\n");
        QVERIFY(QFile::link(dark, current + "/theme"));
        AppTheme theme(current);
        QCOMPARE(theme.rounding(), 0);
        QCOMPARE(theme.colors().value("windowBorder").value<QColor>(), QColor("#7788ff"));
        Deck deck;
        const QString source = deck.source();
        const auto palette = deck.palette();
        QCOMPARE(theme.colors().value("background").value<QColor>(), QColor("#101020"));
        QVERIFY(QFile::remove(current + "/theme"));
        QVERIFY(QFile::link(light, current + "/theme"));
        QTRY_COMPARE(theme.colors().value("background").value<QColor>(), QColor("#ffffff"));
        QCOMPARE(theme.colors().value("foreground").value<QColor>(), QColor("#111111"));
        QCOMPARE(theme.rounding(), 6);
        QCOMPARE(theme.colors().value("accentText").value<QColor>(), QColor("#ffffff"));
        // Atomic file replacement must work repeatedly after a theme switch.
        for (const auto &color : {"#112233", "#334455"}) {
            QSaveFile file(light + "/colors.toml");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray("background = '") + color + "'\n");
            QVERIFY(file.commit());
            QTRY_COMPARE(theme.colors().value("background").value<QColor>(), QColor(color));
        }
        QCOMPARE(deck.source(), source);
        QCOMPARE(deck.palette(), palette);
    }
    void presentationSize() {
        QTemporaryDir files;
        QVERIFY(QDir().mkpath(files.path() + "/images"));
        write(files.path() + "/images/picture.png", "image bytes");
        const QString source = "# Héllo\n---\n![](picture.png)\n---\n![](picture.png)\n---\n![](missing.png)\n";
        write(files.path() + "/talk.md", source);
        Deck deck;
        QVERIFY(deck.loadPath(files.path() + "/talk.md"));
        QCOMPARE(deck.totalBytes(), source.toUtf8().size() + qint64(11));
        deck.select(1);
        QCOMPARE(deck.totalBytes(), source.toUtf8().size() + qint64(11));
        deck.editSource("# Text only\n");
        QCOMPARE(deck.totalBytes(), qint64(12));
        QVERIFY(!deck.sizeLabel().isEmpty());
    }
    void remembersPresentationDirectory() {
        QTemporaryDir files;
        const QString opened = files.path() + "/opened";
        const QString saved = files.path() + "/saved";
        QVERIFY(QDir().mkpath(opened));
        QVERIFY(QDir().mkpath(saved));
        write(opened + "/talk.md", "# Hello\n");
        Deck first;
        QVERIFY(first.loadPath(opened + "/talk.md"));
        Deck next;
        QCOMPARE(next.dialogDirectory(), opened);
        QVERIFY(first.savePath(saved + "/copy.md"));
        QCOMPARE(next.dialogDirectory(), saved);
        QVERIFY(!first.loadPath(files.path() + "/missing/talk.md"));
        QVERIFY(!first.savePath(files.path() + "/missing/copy.md"));
        QCOMPARE(next.dialogDirectory(), saved);
        QSettings persisted(QSettings::IniFormat, QSettings::UserScope, "hype", "hype");
        QCOMPARE(persisted.value("files/lastDirectory").toString(), saved);
        QVERIFY(QFile::exists(persisted.fileName()));
        QVERIFY(QDir(saved).removeRecursively());
        QVERIFY(next.dialogDirectory() != saved);
        QVERIFY(QDir(next.dialogDirectory()).exists());
    }
    void reopensLastPresentation() {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "hype", "hype");
        settings.remove("files/lastPresentation");
        Deck first;
        const QString initial = first.source();
        QVERIFY(!first.reopenLastPresentation());
        QCOMPARE(first.source(), initial);

        QTemporaryDir files;
        const QString original = files.path() + "/original.md";
        const QString copy = files.path() + "/saved copy.md";
        write(original, "# Last presentation\n");
        QVERIFY(first.loadPath(original));
        Deck reopened;
        QVERIFY(reopened.reopenLastPresentation());
        QCOMPARE(reopened.path(), original);
        QCOMPARE(reopened.source(), first.source());
        QVERIFY(!reopened.dirty());

        first.editSlide("# Saved copy\n");
        QVERIFY(first.savePath(copy));
        QVERIFY(!first.loadPath(files.path() + "/missing.md"));
        QVERIFY(!first.savePath(files.path() + "/missing/copy.md"));
        Deck saved;
        QVERIFY(saved.reopenLastPresentation());
        QCOMPARE(saved.path(), copy);
        QCOMPARE(saved.source(), first.source());

        QVERIFY(QFile::remove(copy));
        Deck missing;
        QVERIFY(!missing.reopenLastPresentation());
        QVERIFY(missing.path().isEmpty());
        QCOMPARE(missing.source(), initial);
    }
    void incompleteSlideCodeKeepsFollowingSlides() {
        QTemporaryDir files;
        const QString path = files.path() + "/presentation.md";
        const QString tail = "---\n# Keep this slide\n---\n# And this one\n";
        write(path, "# Start\n---\n# Code goes here\n" + tail);
        Deck deck;
        QVERIFY(deck.loadPath(path));
        deck.select(1);
        deck.editSlide("```rust");
        QVERIFY(deck.source().endsWith(tail));
        deck.editSlide("```rust\nfn main() {}");
        QVERIFY2(deck.source().endsWith(tail), "Editing an unfinished code fence deleted the rest of the presentation");
        QCOMPARE(deck.count(), 4);
        QVERIFY(!deck.savePath(path));
        QFile unchanged(path);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), ("# Start\n---\n# Code goes here\n" + tail).toUtf8());
        deck.undo();
        QCOMPARE(deck.count(), 4);
        deck.redo();
        QCOMPARE(deck.count(), 4);
        deck.editSlide("```rust\nfn main() {}\n```");
        QCOMPARE(deck.count(), 4);
        QVERIFY(deck.source().endsWith(tail));
        QVERIFY(deck.savePath(path));
        Deck reopened;
        QVERIFY(reopened.loadPath(path));
        QCOMPARE(reopened.count(), 4);
        QVERIFY(reopened.source().endsWith(tail));
    }
    void slideBoundariesSurviveIncompleteEdits() {
        Deck deck;
        const QString tail = "# Following code\n```ruby\nputs :hello\n```\n";
        deck.editSource("# Start\n---\n```rust\nfn main() {}\n```\n---\n" + tail + "---\n# Last\n");
        deck.select(1);
        deck.editSlide("```rust\nfn main() {}\n``"); // Delete one closing backtick.
        QCOMPARE(deck.count(), 4);
        QCOMPARE(deck.slide(2), tail);
        deck.select(2);
        deck.editSlide(tail + "A caption");
        QCOMPARE(deck.count(), 4);
        QCOMPARE(deck.slide(3), "# Last\n");
        deck.undo();
        QCOMPARE(deck.slide(2), tail);
        deck.select(1);
        deck.duplicateSlide();
        QCOMPARE(deck.count(), 5);
        QCOMPARE(deck.slide(3).trimmed(), tail.trimmed());
        deck.undo();
        QCOMPARE(deck.count(), 4);
        deck.chooseFont("Monospace");
        deck.editSlide("~~~rust\nfn main() {}\n~~~");
        QCOMPARE(deck.count(), 4);
        QCOMPARE(deck.slide(2), tail);
        QCOMPARE(deck.slide(3), "# Last\n");
        QCOMPARE(parseDeck(deck.source()).slides.size(), 4);
        // Source offsets count UTF-16 positions, not bytes; preserve CRLF tails.
        deck.editSource("# 🍎\r\n---\r\n# Code\r\n---\r\n# café\r\n");
        deck.select(1);
        for (const QString &draft : {QString("~~~"), QString("~~~rust\nfn main() {}"),
                                     QString("~~~rust\nfn main() {}\n~~~")}) {
            deck.editSlide(draft);
            QCOMPARE(deck.count(), 3);
            QCOMPARE(deck.slide(2), "# café\r\n");
        }
    }
    void savedVersionsAreRecoverable() {
        QTemporaryDir files;
        const QString path = files.path() + "/talk.md";
        write(path, "# Original\n");
        Deck deck;
        QVERIFY(deck.loadPath(path));
        deck.editSlide("# Updated");
        QVERIFY(deck.savePath(path));
        QDir backups(files.path() + "/.hype-backups");
        auto names = backups.entryList(QDir::Files);
        QCOMPARE(names.size(), 1);
        QFile backup(backups.filePath(names.first()));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), QByteArray("# Original\n"));
        QVERIFY(deck.savePath(path));
        QCOMPARE(backups.entryList(QDir::Files).size(), 1); // Unchanged saves don't churn history.
        for (int i = 0; i < 22; ++i) {
            deck.editSlide("# Revision " + QString::number(i));
            QVERIFY(deck.savePath(path));
        }
        QCOMPARE(backups.entryList(QDir::Files).size(), 23);
        const QString blocked = files.path() + "/blocked";
        QVERIFY(QDir().mkpath(blocked));
        write(blocked + "/talk.md", "# Keep me\n");
        write(blocked + "/.hype-backups", "A file blocks backup creation");
        QVERIFY(deck.loadPath(blocked + "/talk.md"));
        deck.editSlide("# Replacement");
        QVERIFY(!deck.savePath(deck.path()));
        QFile original(deck.path());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("# Keep me\n"));
        QVERIFY(deck.dirty());
    }
    void autosaveAndVersionHistory() {
        QTemporaryDir files;
        const QString path = files.filePath("talk.md"), recovery = files.filePath("recovery");
        write(path, "# Original\n---\n# Last\n");
        Deck deck;
        QVERIFY(deck.loadPath(path));
        deck.enableAutosave(recovery);
        QCOMPARE(deck.recoveryVersions().size(), 1);
        const auto original = deck.recoveryVersions().first().toMap()["name"].toString();
        deck.editSlide("# Autosaved");
        QTRY_VERIFY_WITH_TIMEOUT(!deck.dirty(), 2500);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), deck.source());
        QVERIFY(deck.recoveryVersions().size() >= 2);
        const QString autosaved = deck.source();
        QVERIFY(deck.restoreVersion(original));
        QCOMPARE(deck.slideText(), "# Original");
        QVERIFY(!deck.dirty());
        bool keptCurrent = false;
        for (const auto &entry : deck.recoveryVersions()) {
            QVERIFY(deck.restoreVersion(entry.toMap()["name"].toString()));
            if (deck.source() == autosaved) keptCurrent = true;
        }
        QVERIFY(keptCurrent);
        QVERIFY(!deck.restoreVersion("../../outside.json"));
    }
    void continuousTypingStillAutosaves() {
        QTemporaryDir files;
        const QString path = files.filePath("talk.md");
        write(path, "# Original\n");
        Deck deck;
        QVERIFY(deck.loadPath(path));
        deck.enableAutosave(files.filePath("recovery"));
        QTimer typing;
        int revision = 0;
        connect(&typing, &QTimer::timeout, &deck, [&] { deck.editSlide("# Revision " + QString::number(++revision)); });
        typing.start(100);
        auto contents = [&] { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return QByteArray(); return f.readAll(); };
        QTRY_VERIFY_WITH_TIMEOUT(contents().contains("Revision"), 6500);
        typing.stop();
        QVERIFY(revision > 10);
        QVERIFY(deck.flushAutosave());
        QCOMPARE(QString::fromUtf8(contents()), deck.source());
    }
    void recoversIncompleteDraftAndSlideBoundaries() {
        QTemporaryDir files;
        const QString path = files.filePath("talk.md"), recovery = files.filePath("recovery");
        const QString original = "# First\n---\n# Middle\n---\n```ruby\nputs 1\n```\n---\n# Last\n";
        write(path, original);
        QString draft;
        {
            Deck deck;
            QVERIFY(deck.loadPath(path));
            deck.enableAutosave(recovery);
            deck.select(1);
            deck.editSlide("```rust\nfn main() {}");
            draft = deck.source();
            QVERIFY(deck.flushAutosave());
            QVERIFY(deck.dirty());
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromUtf8(file.readAll()), original);
        } // No graceful close/save: simulate reopening after the process disappeared.
        Deck restored;
        QVERIFY(restored.loadPath(path));
        restored.enableAutosave(recovery);
        QCOMPARE(restored.source(), draft);
        QCOMPARE(restored.count(), 4);
        QCOMPARE(restored.selected(), 1);
        QVERIFY(restored.dirty());
        restored.editSlide("```rust\nfn main() {}\n```");
        QVERIFY(restored.flushAutosave());
        QVERIFY(!restored.dirty());
        QCOMPARE(restored.count(), 4);
        QCOMPARE(restored.slide(3).trimmed(), "# Last");
        Deck reopened;
        QVERIFY(reopened.loadPath(path));
        QCOMPARE(reopened.count(), 4);
    }
    void recoveryRespectsExternalChanges() {
        QTemporaryDir files;
        const QString path = files.filePath("talk.md"), recovery = files.filePath("recovery");
        write(path, "# Original\n");
        {
            Deck deck;
            QVERIFY(deck.loadPath(path));
            deck.enableAutosave(recovery);
            deck.editSlide("```rust\nunfinished");
            QVERIFY(deck.flushAutosave());
        }
        write(path, "# External edit\n");
        Deck recovered;
        QVERIFY(recovered.loadPath(path));
        recovered.enableAutosave(recovery);
        QVERIFY(recovered.slideText().contains("unfinished"));
        recovered.editSlide("# Recovered and finished");
        QVERIFY(recovered.flushAutosave());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("# External edit\n"));
        QVERIFY(recovered.dirty());
        {
            Deck again;
            QVERIFY(again.loadPath(path)); again.enableAutosave(recovery);
            QVERIFY(again.flushAutosave());
            QFile disk(path); QVERIFY(disk.open(QIODevice::ReadOnly));
            QCOMPARE(disk.readAll(), QByteArray("# External edit\n"));
            QVERIFY(again.dirty());
        }
        // A clean checkpoint must not replace legitimate external edits.
        const QString clean = files.filePath("clean.md");
        write(clean, "# Before\n");
        { Deck deck; QVERIFY(deck.loadPath(clean)); deck.enableAutosave(recovery); }
        write(clean, "# After\n");
        Deck external;
        QVERIFY(external.loadPath(clean)); external.enableAutosave(recovery);
        QCOMPARE(external.slideText(), "# After");
        QVERIFY(!external.dirty());
    }
    void externalEditsKeepTheCurrentSlide() {
        QTemporaryDir files;
        const QString path = files.filePath("talk.md");
        write(path, "# One\n\n---\n\n# Two\n\n---\n\n# Three\n");
        Deck deck;
        QVERIFY(deck.loadPath(path));
        deck.select(1);
        // The slide being viewed changes under the viewer.
        write(path, "# One\n\n---\n\n# Two, revised\n\n---\n\n# Three\n");
        QTRY_COMPARE(deck.slideText(), "# Two, revised");
        QCOMPARE(deck.selected(), 1);
        QVERIFY(!deck.dirty());
        // A slide added earlier moves it; the selection follows.
        write(path, "# Zero\n\n---\n\n# One\n\n---\n\n# Two, revised\n\n---\n\n# Three\n");
        QTRY_COMPARE(deck.count(), 4);
        QCOMPARE(deck.selected(), 2);
        QCOMPARE(deck.slideText(), "# Two, revised");
        // Writers that replace the file, or remove it first, are still followed.
        write(path + ".new", "# Zero\n\n---\n\n# One\n\n---\n\n# Two, again\n\n---\n\n# Three\n");
        QVERIFY(QFile::remove(path));
        QTest::qWait(150);
        QVERIFY(QFile::rename(path + ".new", path));
        QTRY_COMPARE(deck.slideText(), "# Two, again");
        write(path, "# Zero\n\n---\n\n# One\n\n---\n\n# Two, finally\n");
        QTRY_COMPARE(deck.slideText(), "# Two, finally");
        QVERIFY(!deck.dirty());
        deck.undo();
        QCOMPARE(deck.slideText(), "# Two, again");
        // Moved and reworded at once: the closest slide among the changed ones.
        deck.redo();
        write(path, "# New\n\n---\n\n# Zero\n\n---\n\n# One\n\n---\n\n# Two, at last\n\n---\n\n# Coda\n");
        QTRY_COMPARE(deck.slideText(), "# Two, at last");
        QCOMPARE(deck.selected(), 3);
        // Its removal leaves the slide that took its place.
        write(path, "# New\n\n---\n\n# Zero\n\n---\n\n# One\n\n---\n\n# Coda\n");
        QTRY_COMPARE(deck.count(), 4);
        QCOMPARE(deck.slideText(), "# Coda");
        // Unsaved work in the editor is never replaced.
        deck.editSlide("# Mine");
        write(path, "# Theirs\n");
        QTRY_VERIFY(deck.status().contains("Changed on disk"));
        QCOMPARE(deck.slideText(), "# Mine");
    }
    void recoversUntitledAndRefusesUnbackedAutosave() {
        QTemporaryDir files;
        const QString recovery = files.filePath("recovery");
        { Deck draft; draft.enableAutosave(recovery); draft.editSlide("# Unsaved idea"); QVERIFY(draft.flushAutosave()); }
        Deck recovered;
        QVERIFY(!recovered.reopenLastPresentation());
        recovered.enableAutosave(recovery);
        QCOMPARE(recovered.slideText(), "# Unsaved idea");
        QVERIFY(recovered.path().isEmpty());
        QVERIFY(recovered.savePath(files.filePath("named.md")));
        Deck blank; blank.enableAutosave(recovery);
        QVERIFY(blank.slideText() != "# Unsaved idea"); // Save As retires the old unnamed draft.
        const QString path = files.filePath("talk.md");
        write(path, "# Keep me\n");
        write(files.filePath("blocked"), "Not a directory");
        Deck blocked;
        QVERIFY(blocked.loadPath(path)); blocked.enableAutosave(files.filePath("blocked"));
        blocked.editSlide("# New draft");
        QVERIFY(!blocked.flushAutosave());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("# Keep me\n"));
        QVERIFY(blocked.dirty());
    }
    void recoversMissingFileAndDamagedLatestSnapshot() {
        QTemporaryDir files;
        const QString path = files.filePath("talk.md"), recovery = files.filePath("recovery");
        write(path, "# Original\n");
        {
            Deck deck;
            QVERIFY(deck.loadPath(path)); deck.enableAutosave(recovery);
            deck.editSlide("# Latest work"); QVERIFY(deck.flushAutosave());
        }
        QDir root(recovery);
        const auto folders = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(folders.size(), 1);
        write(root.filePath(folders.first() + "/latest.json"), "{broken");
        QVERIFY(QFile::remove(path));
        Deck restored;
        QVERIFY(!restored.reopenLastPresentation());
        restored.enableAutosave(recovery);
        QCOMPARE(restored.path(), path);
        QCOMPARE(restored.slideText(), "# Latest work");
        QVERIFY(restored.dirty());
        QVERIFY(restored.flushAutosave());
        QVERIFY(!QFile::exists(path)); // Missing/externally changed files aren't silently replaced.
        QVERIFY(restored.savePath(files.filePath("restored.md")));
        QVERIFY(!restored.dirty());
    }
    void recoveryHandlesTerminalSeparatorAndNewDeck() {
        QTemporaryDir files;
        const QString recovery = files.filePath("recovery");
        {
            Deck deck; deck.enableAutosave(recovery);
            deck.editSource("# No final newline\n---");
            QVERIFY(deck.flushAutosave());
        }
        const QDir root(recovery);
        const auto folders = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(folders.size(), 1);
        QVERIFY(QFile::remove(root.filePath(folders.first() + "/latest.json")));
        Deck restored; restored.enableAutosave(recovery);
        QCOMPARE(restored.source(), "# No final newline\n---");
        QCOMPARE(restored.count(), 2);
        QVERIFY(restored.status() != "Could not read the recovery draft. Earlier versions are available in History.");
        restored.newDeck();
        const QString blank = restored.source();
        restored.undo();
        QCOMPARE(restored.source(), blank); // Undo never crosses into another presentation.
        Deck latest; latest.enableAutosave(recovery);
        QCOMPARE(latest.source(), blank);
        QVERIFY(latest.recoveryVersions().size() >= 3); // Previous untitled work is still recoverable.
    }
    void historyPopupProtectsSlides() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS")) QSKIP("Set HYPE_GUI_TESTS=1");
        QTemporaryDir files;
        Deck deck; deck.editSource("# First\n---\n# Last\n");
        deck.enableAutosave(files.filePath("recovery"));
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &deck);
        engine.addImageProvider("slides", new Thumbnails(&deck));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto history = window->findChild<QObject *>("historyDialog");
        QVERIFY(history);
        QVERIFY(QMetaObject::invokeMethod(history, "open"));
        QTRY_VERIFY(history->property("visible").toBool());
        QTRY_VERIFY(window->property("popupOpen").toBool());
        const QString source = deck.source();
        QTest::keyClick(window, Qt::Key_Delete);
        QTest::keyClick(window, Qt::Key_D, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(deck.source(), source);
        QCOMPARE(deck.selected(), 0);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!history->property("visible").toBool());
        window->setProperty("allowClose", true);
        window->close();
    }
    void fencesAndFrontMatter() {
        QString source = "---\ntitle: Test\n---\n\n# "
                         "One\n\n---\n\n````ruby\n---\n```\n````\n\n---\n\n~~~sh\n---\n~~~\n";
        auto parsed = parseDeck(source);
        QCOMPARE(parsed.slides.size(), 3);
        QVERIFY(parsed.error.isEmpty());
        QCOMPARE(scalar(parsed.header, "title"), "Test");
        QVERIFY(parsed.slides[1].source.contains("---"));
        for (auto &s : parsed.slides)
            QCOMPARE(source.mid(s.start, s.end - s.start), s.source);
    }
    void emptyBoundariesAndUnicode() {
        auto p = parseDeck("# Æble 🍎\n---\n\n---\n");
        QCOMPARE(p.slides.size(), 3);
        QCOMPARE(p.slides[0].source, QString::fromUtf8("# Æble 🍎\n"));
        QCOMPARE(p.slides[2].source, QString());
    }
    void reorderDuplicateUndoSave() {
        QTemporaryDir tmp;
        QString path = tmp.path() + "/talk.md";
        QString original = "---\ntitle: Test\n---\n\n# One\n\n---\n\n<!-- keep me "
                           "-->\n# Two\n\n---\n\n# Three";
        write(path, original);
        Deck d;
        QVERIFY(d.loadPath(path));
        QString third = d.slide(2), second = d.slide(1);
        d.moveSlide(2, 0);
        QCOMPARE(d.selected(), 0);
        QCOMPARE(d.slide(0).trimmed(), third.trimmed());
        QCOMPARE(d.slide(2).trimmed(), second.trimmed());
        d.undo();
        QCOMPARE(d.source(), original);
        d.redo();
        QCOMPARE(d.slide(0).trimmed(), third.trimmed());
        d.select(2);
        d.duplicateSlide();
        QCOMPARE(d.count(), 4);
        QCOMPARE(d.slide(2).trimmed(), d.slide(3).trimmed());
        d.addSlide();
        QCOMPARE(d.count(), 5);
        QVERIFY(d.slideSource().trimmed().isEmpty());
        d.editSlide("# New");
        QCOMPARE(d.count(), 5);
        QVERIFY(d.savePath(path));
        Deck reopened;
        QVERIFY(reopened.loadPath(path));
        QCOMPARE(reopened.source(), d.source());
        d.undo();
        QVERIFY(d.slideSource().trimmed().isEmpty());
        d.undo();
        QCOMPARE(d.count(), 4);
    }
    void editingCannotEatSeparator() {
        Deck d;
        d.editSource("# One\n---\n# Two\n");
        d.select(0);
        d.editSlide("# Changed");
        QCOMPARE(d.count(), 2);
        QCOMPARE(d.slide(1), QString("# Two\n"));
    }
    void singleSlideEditorPadding() {
        Deck d;
        const QString original =
            "# First\n\n---\n\n\n    indented  \n\nparagraph  \n\n\n---\n\n# Last\n";
        d.editSource(original);
        d.select(1);
        const QString content = "    indented  \n\nparagraph  ";
        QCOMPARE(d.slideText(), content);
        QCOMPARE(d.source(), original); // Viewing a slide doesn't rewrite the document.
        d.editSlide(content + "more");
        QCOMPARE(d.slideText(), content + "more");
        QCOMPARE(d.source(),
                 "# First\n\n---\n\n    indented  \n\nparagraph  more\n\n---\n\n# Last\n");
        QCOMPARE(d.count(), 3);
        d.undo();
        QCOMPARE(d.source(), original);
        d.editSlide("");
        QCOMPARE(d.slideText(), QString());
        QCOMPARE(d.source(), "# First\n\n---\n\n---\n\n# Last\n");
        QCOMPARE(d.count(), 3);
        d.editSource("---\r\ntitle: CRLF\r\n---\r\n\r\n# Title\r\n\r\n---\r\n\r\n# Next\r\n");
        d.select(0);
        QCOMPARE(d.slideText(), "# Title");
        d.editSource("# One slide\n");
        QCOMPARE(d.slideText(), "# One slide");
        d.editSlide("# Changed\n\n");
        QCOMPARE(d.source(), "# Changed\n");
    }
    void slideRangeOperations() {
        Deck d;
        const QString original = "---\ntitle: Ranges\n---\n# A\n---\n# B\n![](photo.png)\n"
                                 "---\n# C\n```text\n---\n```\n---\n# D\n---\n# E\n";
        d.editSource(original);
        const QString b = d.slide(1), c = d.slide(2);
        d.select(1);
        d.extendSelection(3);
        QCOMPARE(d.selectionCount(), 3);
        d.extendSelection(2);
        QCOMPARE(d.selectionCount(), 2);
        QCOMPARE(d.source(), original);
        d.moveSelection(1);
        QCOMPARE(d.selectionFirst(), 2);
        QCOMPARE(d.selectionLast(), 3);
        QCOMPARE(d.selected(), 3);
        QCOMPARE(d.slide(2).trimmed(), b.trimmed());
        QCOMPARE(d.slide(3).trimmed(), c.trimmed());
        d.undo();
        QCOMPARE(d.source(), original);
        QCOMPARE(d.selectionFirst(), 1);
        QCOMPARE(d.selectionLast(), 2);
        d.redo();
        QCOMPARE(d.selectionFirst(), 2);
        d.dropSelection(0);
        QCOMPARE(d.slide(0).trimmed(), b.trimmed());
        QCOMPARE(d.slide(1).trimmed(), c.trimmed());
        QCOMPARE(d.selectionCount(), 2);
        const QString atStart = d.source();
        d.moveSelection(-1);
        d.dropSelection(1);
        QCOMPARE(d.source(), atStart);

        d.select(1);
        d.extendSelection(0); // Preserve the active end of a backward range.
        d.dropSelection(d.count());
        QCOMPARE(d.selected(), 3);
        QCOMPARE(d.selectionLast(), 4);
        QCOMPARE(d.slide(3).trimmed(), b.trimmed());
        QCOMPARE(d.slide(4).trimmed(), c.trimmed());
        d.duplicateSlide();
        QCOMPARE(d.count(), 7);
        QCOMPARE(d.selectionFirst(), 5);
        QCOMPARE(d.selectionLast(), 6);
        QCOMPARE(d.slide(5).trimmed(), b.trimmed());
        QCOMPARE(d.slide(6).trimmed(), c.trimmed());
        d.deleteSlide();
        QCOMPARE(d.count(), 5);
        d.undo();
        QCOMPARE(d.selectionCount(), 2);
        QCOMPARE(d.count(), 7);
        d.select(0);
        d.extendSelection(d.count() - 1);
        d.deleteSlide();
        QCOMPARE(d.count(), 1);
        QCOMPARE(d.selectionCount(), 1);
        QVERIFY(d.slideSource().trimmed().isEmpty());
        d.undo();
        QCOMPARE(d.selectionCount(), 7);
    }
    void deleteLastAndUndo() {
        Deck d;
        QString original = d.source();
        d.deleteSlide();
        QCOMPARE(d.count(), 1);
        QVERIFY(d.slideSource().trimmed().isEmpty());
        d.undo();
        QCOMPARE(d.source(), original);
    }
    void saveDetectsExternalChanges() {
        QTemporaryDir tmp;
        QString path = tmp.path() + "/talk.md";
        write(path, "# Original\n");
        Deck d;
        QVERIFY(d.loadPath(path));
        d.editSlide("# Local\n");
        write(path, "# External\n");
        QVERIFY(!d.savePath(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("# External\n"));
    }
    void mediaDefaultsAndDirectives() {
        auto m = parseMedia("![](<City at night.JPG>)\n\n# Hello", "/tmp/deck");
        QCOMPARE(m.path, QString("/tmp/deck/images/City at night.JPG"));
        QVERIFY(m.span);
        QCOMPARE(m.overlay, .25);
        m = parseMedia("![fit](images/chart.png)\n\n# Chart", "/tmp/deck");
        QVERIFY(!m.span);
        QCOMPARE(m.path, QString("/tmp/deck/images/chart.png"));
        m = parseMedia("![span loop muted poster=\"demo still.jpg\"](demo.MP4)", "/tmp/deck");
        QVERIFY(m.video && m.span && m.loop && m.muted);
        QCOMPARE(m.poster, QString("/tmp/deck/images/demo still.jpg"));
        m = parseMedia("![autoplay=false](demo.mp4)", "/tmp/deck");
        QVERIFY(!m.autoplay);
        m = parseMedia("![span fit](photo.jpg)", "/tmp/deck");
        QVERIFY(!m.error.isEmpty());
        // Side placement is gone; a lone "left" is ordinary alt text again.
        m = parseMedia("![left](photo.jpg)\n\n# Text", "/tmp/deck");
        QVERIFY(m.error.isEmpty());
        QVERIFY(m.span);
    }
    void codeIsNotMedia() {
        QString source = "```markdown\n![](missing.png)\n<!-- Keep this code -->\n```";
        auto media = parseMedia(source, "/tmp");
        QVERIFY(media.file.isEmpty());
        QVERIFY(media.text.contains("<!-- Keep this code -->"));
        QVERIFY(slideProblems(source, "/tmp").isEmpty());
        QVERIFY(parseMedia("An inline `![](missing.png)` example.", "/tmp").file.isEmpty());
    }
    void pasteNamedMedia() {
        QTemporaryDir tmp;
        Deck d;
        d.editSource("# One\n\n---\n\n# Two\n");
        QVERIFY(d.savePath(tmp.path() + "/talk.md"));
        d.select(1);
        const QString before = d.source(), first = d.slide(0);
        QImage image(20, 12, QImage::Format_RGB32);
        image.fill(Qt::red);
        QApplication::clipboard()->setImage(image);
        QSignalSpy request(&d, &Deck::pasteRequested);
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QCOMPARE(request.size(), 1);
        d.cancelPaste();
        QCOMPARE(d.source(), before);
        QVERIFY(!QDir(tmp.path() + "/images").exists());
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QVERIFY(d.savePastedMedia("City at night.png").isEmpty());
        QCOMPARE(QImage(tmp.path() + "/images/City at night.png"), image);
        QCOMPARE(d.slide(0), first);
        QCOMPARE(d.selected(), 1);
        QVERIFY(d.slideSource().contains("# Two"));
        QCOMPARE(parseMedia(d.slideSource(), tmp.path()).file, "City at night.png");
        d.undo();
        QCOMPARE(d.source(), before);

        write(tmp.path() + "/original.webm", "video file bytes");
        auto files = new QMimeData;
        files->setUrls({QUrl::fromLocalFile(tmp.path() + "/original.webm")});
        files->setImageData(image); // File-manager thumbnails must not replace the video.
        QApplication::clipboard()->setMimeData(files);
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QVERIFY(d.savePastedMedia("Demo").isEmpty());
        QFile video(tmp.path() + "/videos/Demo.webm");
        QVERIFY(video.open(QIODevice::ReadOnly));
        QCOMPARE(video.readAll(), QByteArray("video file bytes"));
        QVERIFY(parseMedia(d.slideSource(), tmp.path()).video);
        auto raw = new QMimeData;
        raw->setData("video/mp4", "raw video bytes");
        QApplication::clipboard()->setMimeData(raw);
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QVERIFY(d.savePastedMedia("Second demo").isEmpty());
        QFile rawVideo(tmp.path() + "/videos/Second demo.mp4");
        QVERIFY(rawVideo.open(QIODevice::ReadOnly));
        QCOMPARE(rawVideo.readAll(), QByteArray("raw video bytes"));
        QVERIFY(!d.slideSource().contains("Demo.webm"));
        QCOMPARE(parseMedia(d.slideSource(), tmp.path()).file, "Second demo.mp4");
        QApplication::clipboard()->setText("ordinary text");
        QVERIFY(!d.pasteMedia());
    }
    void pastedImagesUse4kBudget() {
        QTemporaryDir tmp;
        Deck deck;
        QVERIFY(deck.savePath(tmp.path() + "/talk.md"));
        QImage original(4800, 3600, QImage::Format_ARGB32);
        original.fill(QColor(30, 80, 150, 128));
        for (bool span : {false, true}) {
            deck.editSource(span ? "# Background image\n" : "");
            QApplication::clipboard()->setImage(original);
            QVERIFY(deck.pasteMedia());
            QTRY_VERIFY_WITH_TIMEOUT(!deck.compressingImage(), 10000);
            QVERIFY(deck.savePastedMedia(span ? "span" : "fit").isEmpty());
            const auto media = parseMedia(deck.slideSource(), tmp.path());
            const QImage saved(media.path);
            QCOMPARE(saved.size(), span ? QSize(3840, 2880) : QSize(2880, 2160));
            const QColor pixel = saved.pixelColor(100, 100);
            QCOMPARE(pixel.alpha(), 128);
            QVERIFY(qAbs(pixel.red() - 30) <= 1 && qAbs(pixel.green() - 80) <= 1 &&
                    qAbs(pixel.blue() - 150) <= 1); // Premultiplied resampling rounds channels.
            QVERIFY(QFileInfo(media.path).size() < 100000);
        }
        // Copied still files follow the same sizing, without altering the source.
        const QString source = tmp.path() + "/original.png";
        QVERIFY(original.save(source));
        const qint64 originalBytes = QFileInfo(source).size();
        auto files = new QMimeData;
        files->setUrls({QUrl::fromLocalFile(source)});
        QApplication::clipboard()->setMimeData(files);
        deck.editSource("");
        QVERIFY(deck.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!deck.compressingImage(), 10000);
        QVERIFY(deck.savePastedMedia("copied").isEmpty());
        QCOMPARE(QImage(parseMedia(deck.slideSource(), tmp.path()).path).size(), QSize(2880, 2160));
        QCOMPARE(QFileInfo(source).size(), originalBytes);
        QCOMPARE(QImage(source).size(), original.size());
        // Never flatten animation while optimizing a copied image.
        const QString animation = QFINDTESTDATA("fixtures/animated.webp");
        files = new QMimeData;
        files->setUrls({QUrl::fromLocalFile(animation)});
        QApplication::clipboard()->setMimeData(files);
        QVERIFY(deck.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!deck.compressingImage(), 10000);
        QVERIFY(deck.savePastedMedia("animated").isEmpty());
        QFile before(animation), after(tmp.path() + "/images/animated.webp");
        QVERIFY(before.open(QIODevice::ReadOnly));
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), before.readAll());
        QCOMPARE(imageSizeForCanvas(QSize(100, 200), QSize(3840, 2160), true), QSize(100, 200));
    }
    void losslessImageCompression() {
        // Correlated noisy channels benefit from WebP's lossless channel transform.
        QImage original(640, 480, QImage::Format_RGB32);
        quint32 noise = 1;
        for (int y = 0; y < original.height(); ++y)
            for (int x = 0; x < original.width(); ++x) {
                noise = noise * 1664525 + 1013904223;
                original.setPixel(x, y, qRgb(noise >> 24, (noise >> 16) & 255, noise >> 24));
            }
        QString extension;
        const QByteArray encoded = compressedImage(original, &extension);
        QVERIFY(!encoded.isEmpty());
        QCOMPARE(QImage::fromData(encoded).convertToFormat(QImage::Format_RGB32), original);
        QCOMPARE(extension, QString("webp"));
        QVERIFY(encoded.size() < original.sizeInBytes() * 3 / 4);
    }
    void softeningMatchesStraightforwardBoxBlur() {
        // Three clamped horizontal/vertical box passes over premultiplied pixels,
        // written the obvious way. The renderer's faster blur must match it exactly.
        auto reference = [](QImage image, int radius) {
            const int diameter = radius * 2 + 1;
            for (int pass = 0; pass < 6; ++pass) {
                const bool horizontal = pass % 2 == 0;
                QImage output(image.size(), image.format());
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        int sum[4] = {};
                        for (int i = -radius; i <= radius; ++i) {
                            const QRgb color = image.pixel(horizontal ? qBound(0, x + i, image.width() - 1) : x,
                                                           horizontal ? y : qBound(0, y + i, image.height() - 1));
                            sum[0] += qRed(color); sum[1] += qGreen(color);
                            sum[2] += qBlue(color); sum[3] += qAlpha(color);
                        }
                        output.setPixel(x, y, qRgba(sum[0] / diameter, sum[1] / diameter,
                                                    sum[2] / diameter, sum[3] / diameter));
                    }
                image = output;
            }
            return image;
        };
        quint32 noise = 7;
        for (QSize size : {QSize(61, 37), QSize(5, 3)}) {
            QImage image(size, QImage::Format_ARGB32);
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    noise = noise * 1664525 + 1013904223;
                    image.setPixel(x, y, qRgba(noise >> 24, (noise >> 16) & 255, (noise >> 8) & 255,
                                               x % 7 ? noise & 255 : 0));
                }
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            // Softening scales a two-pixel radius at 1080p with the picture: 3 and 4 pixels here.
            QCOMPARE(softenedImage(image, QSizeF(size.width() * 2 / 3.0, 1)), reference(image, 3));
            QCOMPARE(softenedImage(image, QSizeF(size.width() / 2.0, 1)), reference(image, 4));
        }
    }
    void pastedSlidesKeepBalancedSpacing() {
        QTemporaryDir tmp;
        Deck d;
        d.editSource("# Start\n");
        QVERIFY(d.savePath(tmp.path() + "/talk.md"));
        QImage image(10, 10, QImage::Format_RGB32);
        image.fill(Qt::red);
        QApplication::clipboard()->setImage(image);
        d.addSlide();
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QVERIFY(d.savePastedMedia("first").isEmpty());
        d.addSlide();
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QVERIFY(d.savePastedMedia("second").isEmpty());
        d.addSlide();
        d.editSlide("# End");
        const QString expected = "# Start\n\n---\n\n![](<first.png>)\n\n---\n\n"
                                 "![](<second.png>)\n\n---\n\n# End\n";
        QCOMPARE(d.source(), expected);
        d.moveSlide(1, 2);
        QCOMPARE(d.source(), "# Start\n\n---\n\n![](<second.png>)\n\n---\n\n"
                             "![](<first.png>)\n\n---\n\n# End\n");
        d.undo();
        QCOMPARE(d.source(), expected);
        d.select(1);
        QCOMPARE(d.slideText(), "![](<first.png>)");
        d.addSlide();
        QCOMPARE(d.count(), 5);
        QCOMPARE(d.slideText(), QString());
        QVERIFY(d.source().contains("![](<first.png>)\n\n---\n\n---\n\n![](<second.png>)"));
    }
    void pasteValidatesNames() {
        QTemporaryDir tmp;
        Deck d;
        QVERIFY(d.savePath(tmp.path() + "/talk.md"));
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        write(tmp.path() + "/images/existing.png", "keep existing");
        QImage image(10, 10, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QApplication::clipboard()->setImage(image);
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        QVERIFY(d.savePastedMedia("../outside").contains("without folders"));
        QVERIFY(d.savePastedMedia("existing").contains("already exists"));
        QVERIFY(d.savePastedMedia("new name").isEmpty());
        QCOMPARE(QImage(tmp.path() + "/images/new name.png"), image);
        QVERIFY(!QFile::exists(tmp.path() + "/outside.png"));
        QFile existing(tmp.path() + "/images/existing.png");
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep existing"));
        QVERIFY(d.pasteMedia());
        QTRY_VERIFY_WITH_TIMEOUT(!d.compressingImage(), 10000);
        d.editSlide("# Changed while naming media");
        QVERIFY(d.savePastedMedia("wrong slide").contains("slide changed"));
        QVERIFY(!QFile::exists(tmp.path() + "/images/wrong slide.png"));
        d.cancelPaste();
    }
    void replaceSlideMedia() {
        const QString examples = "<!-- ![](comment.png) -->\n```md\n![](example.png)\n```\n"
                                 "Inline `![](inline.png)`\n# Title\n";
        const QString replacement = "![](<new.png>)";
        QCOMPARE(withMedia(examples + "![fit](old.png)\nCaption", replacement),
                 examples + replacement + "\nCaption");
        QCOMPARE(withMedia(examples, replacement), examples + "\n" + replacement + "\n");
    }
    void asynchronousExport() {
        const QString executable = QFINDTESTDATA("../build/hype");
        QVERIFY(!executable.isEmpty());
        QTemporaryDir tmp;
        Deck deck(nullptr, executable);
        QVERIFY(deck.savePath(tmp.path() + "/talk.md"));
        deck.editSource("# Unsaved snapshot\n---\n# Second\n---\n# Third\n");
        QSignalSpy finished(&deck, &Deck::exportFinished);
        int ticks = 0, updates = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
        heartbeat.start(5);
        connect(&deck, &Deck::exportChanged, [&] { ++updates; });
        const QString pdfPath = tmp.path() + "/talk.pdf";
        deck.startExport("pdf", pdfPath);
        QVERIFY(deck.exporting());
        QCOMPARE(finished.size(), 0);
        // Further edits don't affect the in-flight snapshot.
        deck.editSource("# Edited while exporting\n");
        deck.startExport("pdf", tmp.path() + "/duplicate.pdf");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY(finished.last()[0].toBool());
        QVERIFY(!deck.exporting() && !deck.exportFailed());
        QVERIFY(ticks > 1 && updates > 2);
        QCOMPARE(deck.exportProgress(), 1.0);
        QVERIFY(!QFile::exists(tmp.path() + "/duplicate.pdf"));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(pdfPath), QPdfDocument::Error::None);
        QCOMPARE(pdf.pageCount(), 3);
        QVERIFY(pdf.getAllText(0).text().contains("Unsaved snapshot"));
        QCOMPARE(deck.source(), QString("# Edited while exporting\n"));
        QVERIFY(deck.dirty());

        const QString pptxPath = tmp.path() + "/talk.pptx";
        deck.startExport("pptx", pptxPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 15000);
        QVERIFY(finished.last()[0].toBool());
        QFile pptx(pptxPath);
        QVERIFY(pptx.open(QIODevice::ReadOnly));
        const QByteArray original = pptx.readAll();
        pptx.close();
        QVERIFY(original.startsWith("PK"));

        // Cancellation cannot damage an existing successful export.
        deck.startExport("pptx", pptxPath);
        deck.cancelExport();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QVERIFY(!finished.last()[0].toBool());
        QVERIFY(!deck.exporting() && !deck.exportFailed());
        QCOMPARE(deck.exportStatus(), QString("Export cancelled"));
        // Also cancel after the renderer has started reporting progress.
        deck.editSource("# One\n---\n# Two\n---\n# Three\n");
        auto cancelOnProgress = connect(&deck, &Deck::exportChanged, [&] {
            if (deck.exporting() && deck.exportProgress() > 0) deck.cancelExport();
        });
        deck.startExport("pptx", pptxPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 10000);
        disconnect(cancelOnProgress);
        QVERIFY(!finished.last()[0].toBool());
        deck.editSource("![](missing.png)");
        deck.startExport("pptx", pptxPath);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 5, 10000);
        QVERIFY(!finished.last()[0].toBool());
        QVERIFY(deck.exportFailed());
        QVERIFY(deck.exportStatus().contains("missing.png"));
        QVERIFY(pptx.open(QIODevice::ReadOnly));
        QCOMPARE(pptx.readAll(), original);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(QDir(tmp.path()).entryList({".hype-export-*"}, QDir::Dirs | QDir::Hidden).size(), 0);

        Deck unavailable(nullptr, tmp.path() + "/missing-program");
        QSignalSpy failed(&unavailable, &Deck::exportFinished);
        unavailable.startExport("pdf", tmp.path() + "/failed.pdf");
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!failed.last()[0].toBool());
        QVERIFY(!unavailable.exporting() && unavailable.exportFailed());
    }
    void pasteCompressionProgress() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local multimedia access");
        QTemporaryDir files;
        Deck deck;
        QVERIFY(deck.savePath(files.path() + "/talk.md"));
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &deck);
        engine.addImageProvider("slides", new Thumbnails(&deck));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        auto progress = window->findChild<QObject *>("compressionDialog");
        auto naming = window->findChild<QObject *>("pasteDialog");
        QVERIFY(progress && naming);
        QImage image(20, 12, QImage::Format_RGB32);
        image.fill(Qt::red);
        QApplication::clipboard()->setImage(image);
        QSignalSpy requests(&deck, &Deck::pasteRequested);
        QSignalSpy shown(progress, SIGNAL(opened()));

        // Hold the worker queue to exercise the one-second delay deterministically,
        // without relying on machine speed or a deliberately enormous image.
        auto pool = QThreadPool::globalInstance();
        const int threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        auto restore = qScopeGuard([&] { pool->setMaxThreadCount(threads); });
        for (bool cancel : {false, true}) {
            QSemaphore entered, release;
            auto blocker = QtConcurrent::run([&] { entered.release(); release.acquire(); });
            entered.acquire();
            auto unblock = qScopeGuard([&] { release.release(); blocker.waitForFinished(); });
            QVERIFY(deck.pasteMedia());
            QVERIFY(deck.compressingImage());
            QVERIFY(!progress->property("visible").toBool());
            QTest::qWait(400);
            QVERIFY(!progress->property("visible").toBool());
            QTRY_VERIFY_WITH_TIMEOUT(progress->property("opened").toBool(), 1500);
            // The GUI event loop remains available while the worker is pending.
            QVERIFY(!naming->property("visible").toBool());
            if (cancel) {
                QVERIFY(QMetaObject::invokeMethod(progress, "close"));
                QTRY_VERIFY(!deck.compressingImage());
            }
            release.release();
            QTRY_VERIFY_WITH_TIMEOUT(!deck.compressingImage(), 5000);
            if (!cancel) {
                QTRY_VERIFY(naming->property("opened").toBool());
                QCOMPARE(requests.size(), 1);
                QVERIFY(!progress->property("visible").toBool());
                QVERIFY(QMetaObject::invokeMethod(naming, "close"));
                QTRY_VERIFY(!naming->property("visible").toBool());
            } else {
                pool->waitForDone();
                QCoreApplication::processEvents();
                QCOMPARE(requests.size(), 1); // Cancelled completion must not reopen it.
                QVERIFY(!naming->property("visible").toBool());
            }
        }
        const int slowShows = shown.size();
        QVERIFY(deck.pasteMedia());
        QTRY_VERIFY(naming->property("opened").toBool());
        QCOMPARE(requests.size(), 2);
        QCOMPARE(shown.size(), slowShows); // Fast compression never flashes the meter.
        QVERIFY(QMetaObject::invokeMethod(naming, "close"));
        QTRY_VERIFY(!naming->property("visible").toBool());
        QVERIFY(!deck.dirty());
        QVERIFY(!QDir(files.path() + "/images").exists());

        // Changing the document while work is queued must not paste into a new slide.
        QVERIFY(deck.pasteMedia());
        deck.editSource("# Changed while compressing");
        QTRY_VERIFY(!deck.compressingImage());
        QCOMPARE(requests.size(), 2);
        QVERIFY(!naming->property("visible").toBool());
        QVERIFY(deck.status().contains("slide changed"));
    }
    void videoHoldsLastFrameAndReplays() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local multimedia access");
        if (qEnvironmentVariable("QT_QUICK_BACKEND") == "software")
            QSKIP("Video pixel capture requires QT_QUICK_BACKEND=rhi");
        QTemporaryDir files;
        QVERIFY(QDir().mkpath(files.path() + "/videos"));
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i",
            "color=c=blue:s=90x160:d=0.6", "-f", "lavfi", "-i",
            "color=c=red:s=90x160:d=0.6", "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", files.path() + "/videos/demo.mp4"});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        write(files.path() + "/talk.md", "![fit background=blur autoplay=false](demo.mp4)\n---\n# Next");
        Deck deck;
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &deck);
        engine.addImageProvider("slides", new Thumbnails(&deck));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        QVERIFY(deck.loadPath(files.path() + "/talk.md"));
        auto player = window->findChild<QMediaPlayer *>("player");
        auto output = window->findChild<QQuickItem *>("videoOutput");
        QVERIFY(player && output && player->videoSink());
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        QVERIFY(QMetaObject::invokeMethod(window, "togglePresent"));
        auto frameColor = [&]() {
            const QImage frame = player->videoSink()->videoFrame().toImage();
            return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
        };
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QTRY_VERIFY(frameColor().blue() > 240);
        QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 10000);
        QCOMPARE(player->playbackState(), QMediaPlayer::StoppedState);
        QVERIFY(output->isVisible());
        auto held = output->grabToImage();
        QVERIFY(held);
        QTRY_VERIFY(!held->image().isNull());
        auto heldColor = held->image().pixelColor(held->image().width() / 2, held->image().height() / 2);
        QVERIFY2(heldColor.red() > 240, qPrintable(heldColor.name()));
        QTest::qWait(200);
        held = output->grabToImage();
        QTRY_VERIFY(!held->image().isNull());
        QVERIFY(held->image().pixelColor(held->image().width() / 2, held->image().height() / 2).red() > 240);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
        QTRY_VERIFY(frameColor().blue() > 240);
        QVERIFY(player->position() < 600);
        player->pause();
        deck.editSlide("![fit background=blur](demo.mp4)");
        QVERIFY(deck.savePath(files.path() + "/talk.md"));
        QTest::qWait(100);
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
        deck.select(1);
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::StoppedState);
        QVERIFY(!output->isVisible());
        QVERIFY(!player->videoSink()->videoFrame().isValid());
    }
    void animatedImages() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local multimedia access");
        QTemporaryDir files;
        QVERIFY(QDir().mkpath(files.path() + "/images"));
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/animated.webp"),
                            files.path() + "/images/demo.webp"));
        QImage still(64, 48, QImage::Format_RGB32);
        still.fill(Qt::green);
        QVERIFY(still.save(files.path() + "/images/still.webp"));
        write(files.path() + "/talk.md",
              "![fit background=#123456](demo.webp)\n\n---\n\n![](still.webp)\n");
        Deck d;
        QVERIFY(d.loadPath(files.path() + "/talk.md"));
        QVERIFY(d.media()["animated"].toBool());
        // Animation's transparent frames must reveal the background, not its first frame.
        Thumbnails provider(&d);
        const QImage background =
            provider.requestImage(d.renderId(0) + "/background", nullptr, QSize(192, 108));
        QCOMPARE(background.pixelColor(96, 54), QColor("#123456"));
        const QImage thumbnail = provider.requestImage(d.renderId(0), nullptr, QSize(192, 108));
        QCOMPARE(thumbnail.pixelColor(96, 54), QColor(Qt::red));
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &d);
        engine.addImageProvider("slides", new Thumbnails(&d));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        auto loader = window->findChild<QObject *>("animationLoader");
        QVERIFY(loader);
        QTRY_VERIFY(loader->property("item").value<QObject *>());
        auto animation = loader->property("item").value<QObject *>();
        QTRY_COMPARE(animation->property("frameCount").toInt(), 3);
        const int frame = animation->property("currentFrame").toInt();
        QTRY_VERIFY(animation->property("currentFrame").toInt() != frame);
        animation->setProperty("paused", true);
        const int paused = animation->property("currentFrame").toInt();
        QTest::qWait(450);
        QCOMPARE(animation->property("currentFrame").toInt(), paused);
        window->setProperty("markdown", true);
        QTRY_VERIFY(!loader->property("active").toBool());
        QTRY_VERIFY(!loader->property("item").value<QObject *>());
        window->setProperty("markdown", false);
        QTRY_VERIFY(loader->property("item").value<QObject *>());
        d.select(1);
        QVERIFY(!d.media()["animated"].toBool());
        QTRY_VERIFY(!loader->property("active").toBool());
        QTRY_VERIFY(!loader->property("item").value<QObject *>());
        d.select(0);
        d.editSlide("![fit autoplay=false](demo.webp)\n\n# Caption");
        QTRY_VERIFY(loader->property("item").value<QObject *>());
        animation = loader->property("item").value<QObject *>();
        QTRY_VERIFY(animation->property("paused").toBool());
        QVERIFY(!d.media()["span"].toBool());
        window->setProperty("presenting", true);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!animation->property("paused").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(animation->property("paused").toBool());
    }
    void markdownFormatting() {
        QFile script(":/Markdown.js");
        QVERIFY(script.open(QIODevice::ReadOnly));
        QJSEngine engine;
        const auto loaded = engine.evaluate(QString::fromUtf8(script.readAll()).replace(".pragma library", ""));
        QVERIFY2(!loaded.isError(), qPrintable(loaded.toString()));
        const auto format = engine.globalObject().property("format");
        auto edit = [&](const QString &source, int start, int end, const QString &kind) {
            return format.call({source, start, end, kind});
        };
        auto bold = edit("# Hello world", 8, 13, "bold");
        QCOMPARE(bold.property("text").toString(), "# Hello **world**");
        QCOMPARE(bold.property("start").toInt(), 10);
        QCOMPARE(bold.property("end").toInt(), 15);
        auto italic = edit(bold.property("text").toString(), 10, 15, "italic");
        QCOMPARE(italic.property("text").toString(), "# Hello ***world***");
        auto plainBold = edit(italic.property("text").toString(), 11, 16, "italic");
        QCOMPARE(plainBold.property("text").toString(), bold.property("text").toString());
        QCOMPARE(edit("# Hello **world**", 10, 15, "bold").property("text").toString(), "# Hello world");
        auto underline = edit("# Hello world", 8, 13, "underline");
        QCOMPARE(underline.property("text").toString(), "# Hello _world_");
        QCOMPARE(underline.property("start").toInt(), 9);
        QCOMPARE(underline.property("end").toInt(), 14);
        QCOMPARE(edit("# Hello _world_", 9, 14, "underline").property("text").toString(), "# Hello world");
        QCOMPARE(edit("", 0, 0, "underline").property("text").toString(), "_underlined text_");
        QCOMPARE(edit("one\ntwo\nthree", 1, 8, "headline").property("text").toString(), "# one\n# two\nthree");
        QCOMPARE(edit("# one\n# two\nthree", 0, 12, "headline").property("text").toString(), "one\ntwo\nthree");
        QCOMPARE(edit("\nFollowing", 0, 0, "headline").property("text").toString(), "# Headline\nFollowing");
        const auto headline = edit("Hello world", 6, 11, "headline");
        QCOMPARE(headline.property("start").toInt(), 8);
        QCOMPARE(headline.property("end").toInt(), 13);
        const auto headlineCursor = edit("Hello world", 6, 6, "headline");
        QCOMPARE(headlineCursor.property("start").toInt(), 8);
        QCOMPARE(headlineCursor.property("end").toInt(), 8);
        const auto emptyHeadline = edit("", 0, 0, "headline");
        QCOMPARE(emptyHeadline.property("start").toInt(), 2);
        QCOMPARE(emptyHeadline.property("end").toInt(), 10);
        auto comment = edit("Private note", 0, 12, "comment");
        QCOMPARE(comment.property("text").toString(), "<!-- Private note -->");
        QCOMPARE(edit(comment.property("text").toString(), 5, 17, "comment").property("text").toString(), "Private note");
        const QString snippet = "```rust\nfn main() {}\n```";
        const QString fenced = edit(snippet, 0, snippet.size(), "code").property("text").toString();
        QCOMPARE(fenced, "````\n" + snippet + "\n````");
        QVERIFY(parseDeck(fenced + "\n---\n# Following").error.isEmpty());
        QCOMPARE(parseDeck(fenced + "\n---\n# Following").slides.size(), 2);
        auto empty = edit("", 0, 0, "bold");
        QCOMPARE(empty.property("text").toString(), "**bold text**");
        QCOMPARE(empty.property("start").toInt(), 2);
        QCOMPARE(empty.property("end").toInt(), 11);
    }
    void typingCodeInVisualEditorKeepsTheDeck() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local graphics access");
        QTemporaryDir files;
        const QString path = files.path() + "/presentation.md";
        const QString tail = "---\n# Following\n---\n# Last\n";
        write(path, "# Start\n---\n\n" + tail);
        Deck deck;
        QVERIFY(deck.loadPath(path));
        deck.select(1);
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &deck);
        engine.addImageProvider("slides", new Thumbnails(&deck));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto editor = window->findChild<QQuickItem *>("slideEditor");
        QVERIFY(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        const QString code = "```rust\nfn main() {}\n```";
        for (const QChar character : code) {
            QKeyEvent event(QEvent::KeyPress, character == '\n' ? Qt::Key_Return : 0,
                            Qt::NoModifier, QString(character));
            QCoreApplication::sendEvent(window, &event);
            QCOMPARE(deck.count(), 4);
            QVERIFY(deck.source().endsWith(tail));
        }
        QCOMPARE(editor->property("text").toString(), code);
        QCOMPARE(deck.slideText(), code);
        QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!deck.dirty());
        Deck reopened;
        QVERIFY(reopened.loadPath(path));
        QCOMPARE(reopened.count(), 4);
        QVERIFY(reopened.source().endsWith(tail));
        // Toolbar clicks preserve the editor selection and create one deck undo step.
        editor->setProperty("text", "Hello world");
        QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 6), Q_ARG(int, 11)));
        auto boldButton = window->findChild<QQuickItem *>("boldButton");
        QVERIFY(boldButton);
        const QPoint buttonPoint = boldButton->mapToScene(QPointF(boldButton->width() / 2, boldButton->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, buttonPoint);
        QCOMPARE(deck.slideText(), QString("Hello **world**"));
        QVERIFY(editor->hasActiveFocus());
        QCOMPARE(editor->property("selectedText").toString(), QString("world"));
        deck.undo();
        QCOMPARE(deck.slideText(), QString("Hello world"));
        // The formatting hotkeys act on the same selection.
        QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5)));
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QCOMPARE(deck.slideText(), QString("*Hello* world"));
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(deck.slideText(), QString("***Hello*** world"));
        QTest::keyClick(window, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(deck.slideText(), QString("***_Hello_*** world"));
        deck.undo();
        deck.undo();
        deck.undo();
        QCOMPARE(deck.slideText(), QString("Hello world"));
        QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 11)));
        QVERIFY(QMetaObject::invokeMethod(window, "formatSlide", Q_ARG(QVariant, "code")));
        QCOMPARE(deck.slideText(), QString("```\nHello world\n```"));
        QCOMPARE(deck.count(), 4);
        QVERIFY(deck.source().endsWith(tail));
        QVERIFY(deck.savePath(path));
        // Complete slide breaks pasted into this editor resync its visible range.
        editor->setProperty("text", "# First pasted\n---\n# Second pasted");
        QCOMPARE(deck.count(), 5);
        QCOMPARE(editor->property("text").toString(), "# First pasted");
        editor->setProperty("text", "# First pasted and edited");
        QCOMPARE(deck.count(), 5);
        QCOMPARE(deck.slide(2).trimmed(), "# Second pasted");
        QVERIFY(deck.source().endsWith(tail));
        // The same toolbar formats the full document in Markdown mode.
        QVERIFY(QMetaObject::invokeMethod(window, "openMarkdown"));
        auto source = window->findChild<QQuickItem *>("sourceEditor");
        auto sourceBold = window->findChild<QQuickItem *>("source.boldButton");
        QVERIFY(source && sourceBold);
        QTRY_VERIFY(sourceBold->isVisible());
        const QString before = deck.source();
        const int at = before.indexOf("edited");
        QVERIFY(at >= 0);
        QVERIFY(QMetaObject::invokeMethod(source, "select", Q_ARG(int, at), Q_ARG(int, at + 6)));
        const QPoint sourcePoint = sourceBold->mapToScene(QPointF(sourceBold->width() / 2, sourceBold->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sourcePoint);
        QString bolded = before;
        bolded.replace(at, 6, "**edited**");
        QCOMPARE(deck.source(), bolded);
        QCOMPARE(source->property("text").toString(), bolded);
        QCOMPARE(source->property("selectedText").toString(), QString("edited"));
        QCOMPARE(deck.count(), 5);
    }
    void overviewMode() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local graphics access");
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        Deck d;
        QStringList slides;
        for (int i = 1; i <= 12; ++i)
            slides << QString("# S%1\n").arg(i);
        d.editSource(slides.join("\n---\n\n"));
        QCOMPARE(d.count(), 12);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &d);
        engine.addImageProvider("slides", new Thumbnails(&d));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        QVERIFY(window);
        QTest::qWait(300);
        auto grid = window->findChild<QQuickItem *>("overviewGrid");
        auto list = window->findChild<QQuickItem *>("thumbnails");
        QVERIFY(grid && list);
        // Ctrl+M flips the overview on and off, returning to the mode it came from;
        // Ctrl+. flips the Markdown source.
        QCOMPARE(window->property("mode").toString(), QString("visual"));
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("overview"));
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("visual"));
        QTest::keyClick(window, Qt::Key_Period, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("markdown"));
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("overview"));
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("markdown"));
        QTest::keyClick(window, Qt::Key_Period, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("visual"));
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(window->property("mode").toString(), QString("overview"));
        QTRY_VERIFY(grid->isVisible());
        QVERIFY(!list->isVisible());
        QVERIFY(!window->findChild<QQuickItem *>("editorPane")->isVisible());
        QTRY_VERIFY(grid->property("columns").toInt() >= 2);
        const int columns = grid->property("columns").toInt();
        const double cellWidth = grid->property("cellWidth").toDouble(),
                     cellHeight = grid->property("cellHeight").toDouble();
        auto center = [&](int index) {
            return grid->mapToScene(QPointF((index % columns + 0.4) * cellWidth,
                                            (index / columns + 0.4) * cellHeight)).toPoint();
        };
        // Clicking selects, and the arrows move by slide and by row.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(1));
        QCOMPARE(d.selected(), 1);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(d.selected(), 1 + columns);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(d.selected(), columns);
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(d.selected(), 0);
        // Page Down and Page Up jump five rows.
        QTest::keyClick(window, Qt::Key_PageDown);
        QCOMPARE(d.selected(), qMin(d.count() - 1, 5 * columns));
        QTest::keyClick(window, Qt::Key_PageUp);
        QCOMPARE(d.selected(), 0);
        // Ctrl+Down carries the slide a whole row.
        QTest::keyClick(window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(d.selected(), columns);
        QVERIFY(d.slide(columns).contains("# S1\n") || d.slide(columns).trimmed() == "# S1");
        d.undo();
        QVERIFY(d.slide(0).contains("# S1"));
        // Movement by five counts slides even in the grid; a range stays together.
        d.select(2);
        d.extendSelection(3);
        QTest::keyClick(window, Qt::Key_PageDown, Qt::ControlModifier);
        QCOMPARE(d.selectionFirst(), 7);
        QCOMPARE(d.selectionLast(), 8);
        QCOMPARE(d.slide(7).trimmed(), "# S3");
        QCOMPARE(d.slide(8).trimmed(), "# S4");
        QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(d.selectionFirst(), 2);
        QCOMPARE(d.selectionLast(), 3);
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(d.selectionFirst(), 0);
        QCOMPARE(d.selectionLast(), 1);
        QTest::keyClick(window, Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(d.selectionFirst(), 10);
        QCOMPARE(d.selectionLast(), 11);
        QTest::keyClick(window, Qt::Key_PageDown, Qt::ControlModifier);
        QCOMPARE(d.selectionFirst(), 10);
        QCOMPARE(d.selectionLast(), 11);
        for (int i = 0; i < 4; ++i) d.undo();
        QCOMPARE(d.slide(2).trimmed(), "# S3");
        QCOMPARE(d.slide(3).trimmed(), "# S4");
        // Dragging a slide onto the left half of another drops it before that slide.
        d.select(0);
        const QPoint from = center(0), to = center(2) - QPoint(int(cellWidth * 0.3), 0);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 20; ++step)
            QTest::mouseMove(window, from + (to - from) * step / 20, 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(d.selected(), 1);
        QVERIFY(d.slide(0).contains("# S2"));
        QVERIFY(d.slide(1).contains("# S1"));
        QVERIFY(d.slide(2).contains("# S3"));
        // The slide menu opens from the overview too.
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center(1));
        QTRY_VERIFY(window->property("popupOpen").toBool());
        if (qEnvironmentVariableIsSet("HYPE_MENU_SCREENSHOT")) {
            QTest::mouseMove(window, center(1) + QPoint(40, 20));
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("HYPE_MENU_SCREENSHOT")));
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("popupOpen").toBool());
        // The logo opens the shortcuts overlay too.
        auto logo = window->findChild<QQuickItem *>("hypeLogo");
        QVERIFY(logo);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, logo->mapToScene(QPointF(logo->width() / 2, logo->height() / 2)).toPoint());
        QTRY_VERIFY(window->findChild<QObject *>("shortcutsOverlay")->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("popupOpen").toBool());
        // ? toggles the shortcuts overlay. (Ctrl+E and Ctrl+Shift+E open native save
        // dialogs, so they are not pressed here.)
        auto shortcuts = window->findChild<QObject *>("shortcutsOverlay");
        QVERIFY(shortcuts);
        QTest::keyClick(window, Qt::Key_Question, Qt::ShiftModifier);
        QTRY_VERIFY(shortcuts->property("opened").toBool());
        if (qEnvironmentVariableIsSet("HYPE_SHORTCUTS_SCREENSHOT")) {
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("HYPE_SHORTCUTS_SCREENSHOT")));
        }
        QTest::keyClick(window, Qt::Key_Question, Qt::ShiftModifier, 50);
        QTRY_VERIFY(!shortcuts->property("visible").toBool());
        // Clicking a menu button again closes its menu.
        QTest::qWait(250); // Let the Escape above age past the press-to-dismiss window.
        auto fileButton = window->findChild<QQuickItem *>("fileButton");
        auto fileMenu = window->findChild<QObject *>("fileMenu");
        QVERIFY(fileButton && fileMenu);
        const QPoint filePoint = fileButton->mapToScene(QPointF(fileButton->width() / 2, fileButton->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, filePoint);
        QTRY_VERIFY(fileMenu->property("opened").toBool());
        if (qEnvironmentVariableIsSet("HYPE_FILE_MENU_SCREENSHOT")) {
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("HYPE_FILE_MENU_SCREENSHOT")));
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, filePoint, 50);
        QTRY_VERIFY(!fileMenu->property("visible").toBool());
        QTest::qWait(250);
        QVERIFY(!fileMenu->property("visible").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, filePoint);
        QTRY_VERIFY(fileMenu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("popupOpen").toBool());
        // Enter opens the selected slide in Visual mode.
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(window->property("mode").toString(), QString("visual"));
        QTRY_VERIFY(list->isVisible());
        QCOMPARE(d.selected(), 1);
        // An opened presentation starts in the sidebar; a new one starts in the editor.
        auto slideEditor = window->findChild<QQuickItem *>("slideEditor");
        QVERIFY(slideEditor);
        emit d.opened(true);
        QTRY_VERIFY(list->hasActiveFocus());
        emit d.opened(false);
        QTRY_VERIFY(slideEditor->hasActiveFocus());
        QTRY_COMPARE(slideEditor->property("selectedText").toString(), d.slideText().trimmed().mid(2));
        emit d.opened(true);
        QTRY_VERIFY(list->hasActiveFocus());
        // A slide added from the sidebar is ready to type into.
        const int slidesBefore = d.count();
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(d.count(), slidesBefore + 1);
        QTRY_VERIFY(slideEditor->hasActiveFocus());
    }
    void visualOperations() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local multimedia access");
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        Deck d;
        d.editSource("# One\n\n---\n\n# Two\n\n---\n\n# Three\n");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &d);
        engine.addImageProvider("slides", new Thumbnails(&d));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        QVERIFY(window);
        QTest::qWait(300);
        auto list = window->findChild<QQuickItem *>("thumbnails");
        QVERIFY(list);
        const double slideStep = list->property("slideStep").toDouble();
        auto a = list->mapToScene(QPointF(100, 50)).toPoint(),
             b = list->mapToScene(QPointF(100, 2 * slideStep + 90)).toPoint();
        // Hovering a thumbnail names its slide.
        QTest::mouseMove(window, b);
        QTRY_COMPARE(list->property("hoveredSlide").toInt(), 2);
        QTest::mouseMove(window, a);
        QTRY_COMPARE(list->property("hoveredSlide").toInt(), 0);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, a);
        for (int step = 1; step <= 20; ++step)
            QTest::mouseMove(window, a + (b - a) * step / 20, 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, b);
        QCOMPARE(d.selected(), 2);
        QVERIFY(d.slide(2).contains("# One"));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, b);
        for (int step = 1; step <= 20; ++step)
            QTest::mouseMove(window, b + (a - b) * step / 20, 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, a);
        QCOMPARE(d.selected(), 0);
        QVERIFY(d.slide(0).contains("# One"));
        d.undo();
        QCOMPARE(d.selected(), 2);
        QTest::keyClick(window, Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(d.count(), 4);
        QVERIFY(d.slide(3).contains("# One"));
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(d.count(), 5);
        QVERIFY(d.slideSource().trimmed().isEmpty());
        d.undo();
        QCOMPARE(d.count(), 4);
        d.undo();
        QCOMPARE(d.count(), 3);
        d.undo();
        QVERIFY(d.slide(0).contains("# One"));
        QTemporaryDir pasted;
        QVERIFY(d.savePath(pasted.path() + "/talk.md"));
        QImage clipboardImage(12, 12, QImage::Format_RGB32);
        clipboardImage.fill(Qt::green);
        QApplication::clipboard()->setImage(clipboardImage);
        auto pasteTarget = window->findChild<QQuickItem *>("stage");
        pasteTarget->forceActiveFocus();
        auto pasteDialog = window->findChild<QObject *>("pasteDialog");
        auto pasteName = window->findChild<QQuickItem *>("pasteName");
        QVERIFY(pasteDialog && pasteName);
        auto submitName = [&](const QString &name) {
            QTRY_VERIFY(pasteDialog->property("opened").toBool());
            QVERIFY(pasteName->hasActiveFocus());
            pasteName->setProperty("text", name);
            QTest::keyClick(window, Qt::Key_Return);
            QTRY_VERIFY(!pasteDialog->property("visible").toBool());
        };
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(pasteDialog->property("opened").toBool());
        auto frame = window->findChild<QQuickItem *>("slideFrame");
        auto popupItem = pasteDialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(frame && popupItem);
        const QPointF frameCenter =
            frame->mapToScene(QPointF(frame->width() / 2, frame->height() / 2));
        const QPointF popupCenter =
            popupItem->mapToScene(QPointF(popupItem->width() / 2, popupItem->height() / 2));
        QVERIFY(QLineF(frameCenter, popupCenter).length() < 2);
        const QSize previousSize = window->size();
        window->resize(previousSize.width() + 120, previousSize.height() + 100);
        QTRY_VERIFY(QLineF(frame->mapToScene(QPointF(frame->width() / 2, frame->height() / 2)),
            popupItem->mapToScene(QPointF(popupItem->width() / 2, popupItem->height() / 2))).length() < 2);
        window->resize(previousSize);
        const int pasteSlide = d.selected();
        QTest::keyClick(window, Qt::Key_PageDown);
        QCOMPARE(d.selected(), pasteSlide);
        QTest::keyClick(window, Qt::Key_Period, Qt::ControlModifier);
        QVERIFY(!window->property("markdown").toBool());
        if (qEnvironmentVariableIsSet("HYPE_PASTE_SCREENSHOT")) {
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("HYPE_PASTE_SCREENSHOT")));
        }
        submitName("canvas");
        QCOMPARE(parseMedia(d.slideSource(), pasted.path()).file, "canvas.png");
        QTRY_VERIFY(pasteTarget->hasActiveFocus());
        const QString beforeCancel = d.source();
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY(pasteDialog->property("opened").toBool());
        pasteName->setProperty("text", "canvas");
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(pasteDialog->property("visible").toBool());
        QVERIFY(pasteDialog->property("error").toString().contains("already exists"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!pasteDialog->property("visible").toBool());
        QTRY_VERIFY(pasteTarget->hasActiveFocus());
        QCOMPARE(d.source(), beforeCancel);
        auto slideText = window->findChild<QQuickItem *>("slideEditor");
        slideText->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
        submitName("slide editor");
        QCOMPARE(parseMedia(d.slideSource(), pasted.path()).file, "slide editor.png");
        QVERIFY(QMetaObject::invokeMethod(window, "openMarkdown"));
        QTest::qWait(50);
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
        submitName("document editor");
        QCOMPARE(parseMedia(d.slideSource(), pasted.path()).file, "document editor.png");
        auto documentText = window->findChild<QQuickItem *>("sourceEditor");
        documentText->forceActiveFocus();
        QApplication::clipboard()->setText("ordinary paste");
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(d.source().contains("ordinary paste"));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        QTest::keyClick(window, Qt::Key_Period, Qt::ControlModifier);
        QString trial = QFINDTESTDATA("../trials/rails-world-2023/presentation.md");
        if (!trial.isEmpty()) {
            QVERIFY(d.loadPath(trial));
            d.select(48);
            auto player = window->findChild<QObject *>("player");
            QVERIFY(player);
            QVERIFY(QMetaObject::invokeMethod(player, "play"));
            QTRY_COMPARE_WITH_TIMEOUT(player->property("playbackState").toInt(), 1, 10000);
            d.select(49);
            QTRY_COMPARE(player->property("playbackState").toInt(), 0);
        }
        QString many;
        for (int i = 0; i < 40; ++i)
            many += (i ? "\n\n---\n\n" : "") + QString("# Slide %1").arg(i);
        many += "\n";
        d.editSource(many);
        d.select(0);
        QTest::qWait(100);
        const auto dragStart = list->mapToScene(QPointF(100, 50)).toPoint();
        const auto dragEnd = list->mapToScene(QPointF(100, list->height() - 10)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, dragStart);
        for (int step = 1; step <= 20; ++step)
            QTest::mouseMove(window, dragStart + (dragEnd - dragStart) * step / 20, 10);
        QTRY_VERIFY_WITH_TIMEOUT(list->property("contentY").toDouble() > 900, 5000);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragEnd);
        QVERIFY(d.selected() > 5);
        QCOMPARE(d.slideSource().trimmed(), "# Slide 0");
        QCOMPARE(d.count(), 40);
        d.undo();
        QCOMPARE(d.source(), many);
        QCOMPARE(d.selected(), 0);
        auto stage = window->findChild<QQuickItem *>("stage");
        stage->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(d.selected(), 1);
        QCOMPARE(d.slideSource().trimmed(), "# Slide 0");
        QTest::keyClick(window, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(d.selected(), 2);
        QCOMPARE(d.slideSource().trimmed(), "# Slide 0");
        QTest::keyClick(window, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(d.selected(), 1);
        QTest::keyClick(window, Qt::Key_Left, Qt::ControlModifier);
        QCOMPARE(d.selected(), 0);
        QCOMPARE(d.source(), many);
        QTest::keyClick(window, Qt::Key_PageDown, Qt::ControlModifier);
        QCOMPARE(d.selected(), 5);
        QCOMPARE(d.slideSource().trimmed(), "# Slide 0");
        QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(d.selected(), 0);
        QTest::keyClick(window, Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(d.selected(), d.count() - 1);
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(d.selected(), 0);
        QCOMPARE(d.source(), many);
        QTest::keyClick(window, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(d.selected(), 0);
        QCOMPARE(d.source(), many);
        d.select(d.count() - 1);
        QTest::keyClick(window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(d.selected(), d.count() - 1);
        QCOMPARE(d.source(), many);
        d.select(0);
        QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(d.selectionCount(), 2);
        QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(d.selectionCount(), 3);
        QCOMPARE(d.selected(), 2);
        QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(d.selectionCount(), 2);
        QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
        QCOMPARE(d.selectionCount(), 1);
        QCOMPARE(d.source(), many);
        QTest::qWait(100);
        const auto thirdSlide = list->mapToScene(QPointF(100, 2 * slideStep + 50)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, thirdSlide);
        QCOMPARE(d.selectionFirst(), 0);
        QCOMPARE(d.selectionLast(), 2);
        QCOMPARE(d.source(), many);
        QVERIFY(list->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(d.selectionFirst(), 1);
        QCOMPARE(d.selectionLast(), 3);
        QCOMPARE(d.slide(1).trimmed(), "# Slide 0");
        d.undo();
        QCOMPARE(d.source(), many);
        QCOMPARE(d.selectionCount(), 3);
        const auto groupStart = list->mapToScene(QPointF(100, slideStep + 50)).toPoint();
        const auto groupEnd = list->mapToScene(QPointF(100, 4 * slideStep - 30)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, groupStart);
        for (int step = 1; step <= 20; ++step)
            QTest::mouseMove(window, groupStart + (groupEnd - groupStart) * step / 20, 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, groupEnd);
        QCOMPARE(d.selectionFirst(), 1);
        QCOMPARE(d.selectionLast(), 3);
        QCOMPARE(d.slide(1).trimmed(), "# Slide 0");
        QCOMPARE(d.slide(2).trimmed(), "# Slide 1");
        QCOMPARE(d.slide(3).trimmed(), "# Slide 2");
        d.undo();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, groupStart);
        QCOMPARE(d.selectionCount(), 1);
        QCOMPARE(d.selected(), 1);
        d.select(0);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(d.selected(), 1);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(d.selected(), 0);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(d.selected(), 1);
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(d.selected(), 0);
        QTest::keyClick(window, Qt::Key_PageDown);
        QCOMPARE(d.selected(), 5);
        QTest::keyClick(window, Qt::Key_PageUp);
        QCOMPARE(d.selected(), 0);
        d.select(2);
        QTest::keyClick(window, Qt::Key_PageUp);
        QCOMPARE(d.selected(), 0);
        d.select(d.count() - 3);
        QTest::keyClick(window, Qt::Key_PageDown);
        QCOMPARE(d.selected(), d.count() - 1);
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(d.selected(), d.count() - 1);
        QTest::keyClick(window, Qt::Key_PageDown);
        QCOMPARE(d.selected(), d.count() - 1);
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(d.selected(), 0);
        QTest::qWait(100);
        auto point = list->mapToScene(QPointF(100, 70));
        auto wheel = [&](int angle, int pixels = 0) {
            QWheelEvent event(point, window->mapToGlobal(point.toPoint()), QPoint(0, pixels),
                              QPoint(0, angle), Qt::NoButton, Qt::NoModifier,
                              pixels ? Qt::ScrollUpdate : Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &event);
        };
        QPointingDevice touchpad(
            "Test touchpad", 1001, QInputDevice::DeviceType::TouchPad,
            QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 5, 0);
        QWheelEvent touchpadWheel(point, window->mapToGlobal(point.toPoint()),
                                  QPoint(0, -qRound(slideStep)), QPoint(0, -120), Qt::NoButton,
                                  Qt::NoModifier, Qt::ScrollUpdate, false,
                                  Qt::MouseEventSynthesizedBySystem, &touchpad);
        QCoreApplication::sendEvent(window, &touchpadWheel);
        QCOMPARE(d.selected(), 1);
        QWheelEvent pixelWheel(point, window->mapToGlobal(point.toPoint()),
                               QPoint(0, -qRound(slideStep)), QPoint(), Qt::NoButton,
                               Qt::NoModifier, Qt::ScrollUpdate, false,
                               Qt::MouseEventSynthesizedBySystem, &touchpad);
        QCoreApplication::sendEvent(window, &pixelWheel);
        QCOMPARE(d.selected(), 2);
        d.select(0);
        QTest::qWait(60);
        wheel(-120);
        QCOMPARE(d.selected(), 1);
        wheel(-120);
        wheel(-120);
        wheel(-60);
        QCOMPARE(d.selected(), 3);
        wheel(-60);
        QCOMPARE(d.selected(), 4);
        wheel(120);
        QCOMPARE(d.selected(), 3);
        wheel(-120, -17);
        QCOMPARE(d.selected(), 4);
        wheel(12000);
        QCOMPARE(d.selected(), 0);
        wheel(-12000);
        QCOMPARE(d.selected(), d.count() - 1);
        QTest::qWait(60);
        QVERIFY(list->property("contentY").toDouble() > 0);
        d.select(20);
        auto editor = window->findChild<QQuickItem *>("slideEditor");
        QVERIFY(editor && editor->isVisible() && stage->isVisible());
        editor->forceActiveFocus();
        QVERIFY(editor->hasActiveFocus());
        const QString beforeJump = d.source();
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(d.selected(), d.count() - 1);
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(d.selected(), 0);
        QCOMPARE(d.source(), beforeJump);
        d.select(20);
        const QString beforeTab = d.source();
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(list->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(editor->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab, Qt::ShiftModifier);
        QVERIFY(list->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab, Qt::ShiftModifier);
        QVERIFY(editor->hasActiveFocus());
        QCOMPARE(d.source(), beforeTab);
        QCOMPARE(editor->property("text").toString(), d.slideText());
        QVERIFY(editor->mapToScene(QPointF()).y() >=
                stage->mapToScene(QPointF(0, stage->height())).y());
        QString before = d.slideText();
        QTest::keyClick(window, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_X);
        QCOMPARE(d.slideSource().trimmed(), (before + "x").trimmed());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), before + "x\n\n");
        QTest::keyClick(window, Qt::Key_Y);
        QCOMPARE(editor->property("text").toString(), before + "x\n\ny");
        QCOMPARE(d.slideText(), before + "x\n\ny");
        QCOMPARE(editor->property("cursorPosition").toInt(), d.slideText().size());
        QString edited = d.source();
        d.select(21); // Selection while editing must not write the new slide over the old one.
        QCOMPARE(d.source(), edited);
        QCOMPARE(editor->property("text").toString(), d.slideText());
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
        int selected = d.selected();
        const QString beforeShift = d.source();
        QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(d.selected(), selected);
        QCOMPARE(d.source(), beforeShift);
        QVERIFY(editor->property("selectedText").toString().size() > 0);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(d.selected(), selected);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(d.selected(), selected);
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(d.selected(), selected);
        QTest::keyClick(window, Qt::Key_PageDown);
        QCOMPARE(d.selected(), selected);
        QTest::keyClick(window, Qt::Key_PageUp);
        QCOMPARE(d.selected(), selected);
        QString beforeToggle = d.source();
        QTest::keyClick(window, Qt::Key_Period, Qt::ControlModifier);
        QVERIFY(window->property("markdown").toBool());
        auto source = window->findChild<QQuickItem *>("sourceEditor");
        QVERIFY(source && source->isVisible() && source->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(list->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(source->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab, Qt::ShiftModifier);
        QVERIFY(list->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Tab, Qt::ShiftModifier);
        QVERIFY(source->hasActiveFocus());
        QCOMPARE(d.source(), beforeToggle);
        QVERIFY(!stage->isVisible());
        QCOMPARE(source->property("text").toString(), d.source());
        QCOMPARE(source->property("cursorPosition").toInt(), d.sourcePosition());
        QTest::qWait(60);
        auto scroll = window->findChild<QObject *>("sourceScroll");
        auto flick = scroll->property("contentItem").value<QObject *>();
        QRectF rect;
        QVERIFY(QMetaObject::invokeMethod(source, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                          Q_ARG(int, d.sourcePosition())));
        QVERIFY(qAbs(flick->property("contentY").toDouble() - rect.y() +
                     source->property("topPadding").toDouble()) < 2);
        QCOMPARE(d.source(), beforeToggle);
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(source->property("cursorPosition").toInt(), 0);
        QTest::keyClick(window, Qt::Key_PageDown);
        QVERIFY(source->property("cursorPosition").toInt() > 0);
        QVERIFY(flick->property("contentY").toDouble() > 0);
        QTest::keyClick(window, Qt::Key_PageUp);
        QCOMPARE(source->property("cursorPosition").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(source->property("cursorPosition").toInt(), d.source().indexOf('\n'));
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(source->property("cursorPosition").toInt(), 0);
        QTest::keyClick(window, Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(source->property("cursorPosition").toInt(), d.source().size());
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QTest::qWait(60);
        auto sourceFlick = window->findChild<QQuickItem *>("sourceFlick");
        QPointF sourcePoint = sourceFlick->mapToScene(QPointF(100, 100));
        double initialScroll = flick->property("contentY").toDouble();
        QWheelEvent textWheel(sourcePoint, window->mapToGlobal(sourcePoint.toPoint()), QPoint(),
                              QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                              false);
        QCoreApplication::sendEvent(window, &textWheel);
        QCOMPARE(flick->property("contentY").toDouble(), initialScroll + 180);
        QCOMPARE(d.source(), beforeToggle);

        QTest::keyClick(window, Qt::Key_Period, Qt::ControlModifier);
        QVERIFY(!window->property("markdown").toBool());
        QVERIFY(stage->isVisible() && editor->isVisible());
        QVERIFY(stage->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        QVERIFY(window->property("presenting").toBool());
        QVERIFY(!window->findChild<QQuickItem *>("editorPane")->isVisible());
        QVERIFY(stage->isVisible());
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        QVERIFY(!window->property("presenting").toBool());
        QVERIFY(editor->isVisible());
        QString trial2025 = QFINDTESTDATA("../trials/rails-world-2025/presentation.md");
        if (!trial2025.isEmpty()) {
            QVERIFY(d.loadPath(trial2025));
            for (int slide : {1, 60, 121}) {
                d.select(slide);
                QVERIFY(QMetaObject::invokeMethod(window, "openMarkdown"));
                QTest::qWait(100);
                QCOMPARE(source->property("text").toString(), d.source());
                QImage screenshot = window->grabWindow();
                auto sourceFlick = window->findChild<QQuickItem *>("sourceFlick");
                QVERIFY(sourceFlick);
                QRect area(sourceFlick->mapToScene(QPointF(40, 30)).toPoint(), QSize(700, 300));
                int textPixels = 0;
                for (int y = area.top(); y < area.bottom(); ++y)
                    for (int x = area.left(); x < area.right(); ++x) {
                        QColor pixel = screenshot.pixelColor(x, y);
                        if (pixel.red() > 120 && pixel.green() > 120 && pixel.blue() > 120)
                            ++textPixels;
                    }
                QVERIFY2(textPixels > 200,
                         "Scrolled Markdown viewport must draw source text, not a blank pane");
            }
        }
        d.editSource("# Short document\n");
        QVERIFY(QMetaObject::invokeMethod(window, "openMarkdown"));
        QTest::qWait(60);
        for (int i = 0; i < 3; ++i) {
            QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
            QTest::qWait(60);
            QCOMPARE(d.count(), i + 2);
            QCOMPARE(flick->property("contentY").toDouble(), 0.0);
            QVERIFY(flick->property("contentHeight").toDouble() <
                    flick->property("height").toDouble());
        }
        d.editSource(many);
        d.select(10);
        QVERIFY(QMetaObject::invokeMethod(window, "openMarkdown"));
        QTest::qWait(60);
        double middleY = flick->property("contentY").toDouble();
        QVERIFY(QMetaObject::invokeMethod(window, "addSlide"));
        QTest::qWait(60);
        QCOMPARE(flick->property("contentY").toDouble(), middleY);
        d.select(d.count() - 1);
        QVERIFY(QMetaObject::invokeMethod(window, "openMarkdown"));
        QTest::qWait(60);
        double bottomY = flick->property("contentY").toDouble();
        QVERIFY(QMetaObject::invokeMethod(window, "addSlide"));
        QTest::qWait(60);
        double addedScroll = flick->property("contentY").toDouble() - bottomY;
        QVERIFY(addedScroll >= 0 && addedScroll < 150);
        QVERIFY(QMetaObject::invokeMethod(source, "positionToRectangle", Q_RETURN_ARG(QRectF, rect),
                                          Q_ARG(int, d.sourcePosition())));
        double cursorBottom = rect.bottom() - flick->property("contentY").toDouble();
        QVERIFY(cursorBottom > 0 && cursorBottom <= flick->property("height").toDouble());

        // Adding a visible slide must preserve the sidebar viewport, including
        // when the model resets well below the start of a long presentation.
        window->setProperty("markdown", false);
        d.editSource(many);
        d.select(15);
        QTest::qWait(60);
        list->setProperty("contentY", list->property("originY").toDouble() + 12 * slideStep);
        QTest::qWait(60);
        const double sidebarY = list->property("contentY").toDouble() - list->property("originY").toDouble();
        QVERIFY(QMetaObject::invokeMethod(window, "addSlide"));
        QTest::qWait(100);
        QCOMPARE(d.selected(), 16);
        QCOMPARE(list->property("contentY").toDouble() - list->property("originY").toDouble(), sidebarY);

        // When insertion falls below the viewport, reveal only the overflow.
        const int lastVisible = int((sidebarY + list->height() - 108) / slideStep);
        d.select(lastVisible);
        QTest::qWait(60);
        const double beforeInsert = list->property("contentY").toDouble() - list->property("originY").toDouble();
        const double expected = qMax(beforeInsert, (lastVisible + 1) * slideStep + 108 - list->height());
        QVERIFY(QMetaObject::invokeMethod(window, "addSlide"));
        QTest::qWait(100);
        const double afterInsert = list->property("contentY").toDouble() - list->property("originY").toDouble();
        QVERIFY(qAbs(afterInsert - expected) < 1);
        QVERIFY(afterInsert - beforeInsert <= slideStep);
        window->setProperty("allowClose", true);
        window->close();
    }
    void exportRejectsIncompleteSlideStructure() {
        QTemporaryDir tmp;
        Deck d;
        d.editSource("# One\n---\n# Two\n---\n# Three\n");
        d.select(1);
        d.editSlide("```rust\nfn main() {}");
        QCOMPARE(d.count(), 3);
        const QString output = tmp.filePath("existing.pdf");
        write(output, "keep me");
        QVERIFY(!d.exportPdf(output));
        QVERIFY(!d.exportPptx(tmp.filePath("out.pptx")));
        QVERIFY(!d.renderImages(tmp.filePath("rendered")));
        QSignalSpy finished(&d, &Deck::exportFinished);
        d.startExport("pdf", output);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
        QVERIFY(d.exportStatus().contains("Unclosed code fence"));
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("keep me"));
        const auto snapshot = QJsonDocument(QJsonObject{{"source", d.source()}, {"path", tmp.filePath("p.md")}}).toJson();
        write(tmp.filePath("snapshot.json"), QString::fromUtf8(snapshot));
        Deck worker;
        QVERIFY(!worker.loadExportSnapshot(tmp.filePath("snapshot.json")));
        QCOMPARE(d.count(), 3);
        d.editSlide("```rust\nfn main() {}\n```");
        QVERIFY(d.exportPdf(tmp.filePath("complete.pdf")));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(tmp.filePath("complete.pdf")), QPdfDocument::Error::None);
        QCOMPARE(pdf.pageCount(), 3);
    }
    void saveCopyRollsBackOnFailure() {
        QTemporaryDir tmp;
        const QString original = tmp.filePath("original"), destination = tmp.filePath("copy");
        QDir().mkpath(original + "/images");
        QDir().mkpath(destination + "/images");
        write(original + "/images/a.png", "first");
        write(original + "/images/z.png", "last");
        write(destination + "/images/z.png", "different");
        Deck d;
        QVERIFY(d.savePath(original + "/presentation.md"));
        QVERIFY(!d.saveCopyPath(destination + "/presentation.md"));
        QVERIFY(!QFile::exists(destination + "/images/a.png"));
        QFile::remove(destination + "/images/z.png");
        QDir().mkdir(destination + "/blocked.md");
        QVERIFY(!d.saveCopyPath(destination + "/blocked.md"));
        QVERIFY(!QFile::exists(destination + "/images/a.png"));
        QVERIFY(!QFile::exists(destination + "/images/z.png"));
        QCOMPARE(d.path(), original + "/presentation.md");
        QVERIFY(d.saveCopyPath(destination + "/presentation.md"));
        QVERIFY(QFile::exists(destination + "/images/a.png"));
        QCOMPARE(d.path(), destination + "/presentation.md");
    }
    void mediaControlsPreserveCodeAndAltText() {
        Deck d;
        const QString examples = "```markdown\n![](fenced.png)\n```\n"
            "    ![](indented.png)\n\n`![](inline.png)`\n<!-- ![](comment.png) -->\n";
        d.editSlide(examples + "![fit alt=\"fit left right span loop\"](photo.png)");
        d.setMediaMode("span");
        QVERIFY(d.slideText().startsWith(examples));
        QVERIFY(d.slideText().endsWith("![span alt=\"fit left right span loop\"](photo.png)"));
        d.setMediaBackground("blur");
        QVERIFY(d.slideText().contains("alt=\"fit left right span loop\""));
        QVERIFY(d.slideText().startsWith(examples));
        d.editSlide("# Headline\n![span](photo.png)");
        d.setMediaBackground("white");
        QVERIFY(d.slideText().endsWith("![fit background=white](photo.png)"));
        QVERIFY(parseMedia(d.slideText(), {}).error.isEmpty());
        QVERIFY(!parseMedia(d.slideText(), {}).span);
        d.setMediaBackground("black");
        QVERIFY(d.slideText().endsWith("![fit background=black](photo.png)"));
        d.setMediaBackground("theme");
        QCOMPARE(parseMedia(d.slideText(), {}).background, QString("theme"));
        d.editSlide("![Sloop rigging](photo.png)");
        d.setMediaMode("fit");
        QVERIFY(!parseMedia(d.slideText(), {}).span);
        QVERIFY(parseMedia(d.slideText(), {}).error.isEmpty());
        QVERIFY(d.slideText().contains("alt=\"Sloop rigging\""));
        QVERIFY(!parseMedia("```sh\n# Comment\n```\n![](photo.png)", {}).span);
        QVERIFY(parseMedia("# Headline\n![](photo.png)", {}).span);
        const QString original = d.source();
        d.setMediaMode("invalid");
        QCOMPARE(d.source(), original);
    }
    void mediaImportCollisionAndPortability() {
        QTemporaryDir tmp;
        QDir().mkdir(tmp.path() + "/deck");
        QDir().mkdir(tmp.path() + "/source");
        Deck d;
        QVERIFY(d.savePath(tmp.path() + "/deck/presentation.md"));
        QImage a(10, 10, QImage::Format_RGB32);
        a.fill(Qt::red);
        a.save(tmp.path() + "/source/photo.png");
        d.importMedia(QUrl::fromLocalFile(tmp.path() + "/source/photo.png"));
        d.importMedia(QUrl::fromLocalFile(tmp.path() + "/source/photo.png"));
        QCOMPARE(QDir(tmp.path() + "/deck/images").entryList(QDir::Files).size(), 1);
        a.fill(Qt::blue);
        a.save(tmp.path() + "/source/photo.png");
        d.importMedia(QUrl::fromLocalFile(tmp.path() + "/source/photo.png"));
        QCOMPARE(QDir(tmp.path() + "/deck/images").entryList(QDir::Files).size(), 2);
        QVERIFY(d.source().contains("photo-2.png"));
        QVERIFY(slideProblems(d.slideSource(), d.baseDir()).isEmpty());
        QCOMPARE(d.slideSource().count("![]("), 1);
        QVERIFY(d.importMedia(QUrl::fromLocalFile(tmp.path() + "/source/photo.png"), true));
        QCOMPARE(d.count(), 2);
        for (int i = 0; i < d.count(); ++i) QVERIFY(slideProblems(d.slide(i), d.baseDir()).isEmpty());
        write(tmp.path() + "/source/invalid.png", "not an image");
        QVERIFY(!d.importMedia(QUrl::fromLocalFile(tmp.path() + "/source/invalid.png"), true));
        QCOMPARE(d.count(), 2);
    }
    void fontSelection() {
        Deck d;
        QVERIFY(!d.fontNames().isEmpty());
        const QString original = d.source();
        QString family = d.fontNames().first();
        if (family == d.fontName())
            family = d.fontNames().last();
        d.chooseFont(family);
        QCOMPARE(d.fontName(), family);
        QCOMPARE(d.palette()["font"].toString(), family);
        QString changed = d.source();
        d.chooseFont("No such installed font");
        QCOMPARE(d.source(), changed);
        QTemporaryDir tmp;
        QVERIFY(d.savePath(tmp.path() + "/font.md"));
        Deck reopened;
        QVERIFY(reopened.loadPath(tmp.path() + "/font.md"));
        QCOMPARE(reopened.fontName(), family);
        d.undo();
        QCOMPARE(d.source(), original);
    }
    void themeSnapshot() {
        Deck d;
        if (!d.themeNames().contains("tokyo-night"))
            QSKIP("Tokyo Night is not installed");
        d.chooseTheme("tokyo-night");
        QVERIFY(d.source().contains("color_background:"));
        QCOMPARE(d.background(), QColor("#1a1b26"));
        QString before = d.source();
        d.chooseTheme("nord");
        d.undo();
        QCOMPARE(d.source(), before);
    }
    void shutdownDrainsAndRejectsRenders() {
        Deck deck;
        QStringList slides;
        for (int i = 0; i < 80; ++i)
            slides << QString("# Shutdown %1\n").arg(i) + QString("A long line to lay out.\n").repeated(200);
        deck.editSource(slides.join("\n---\n"));
        Thumbnails provider(&deck);
        std::vector<std::unique_ptr<QQuickImageResponse>> responses;
        for (int i = 0; i < deck.count(); ++i)
            responses.emplace_back(provider.requestImageResponse(deck.renderId(i),
                i % 2 ? QSize(340, 192) : QSize(1920, 1080)));
        QSignalSpy tailFinished(responses.back().get(), &QQuickImageResponse::finished);
        provider.shutdown();
        QCOMPARE(tailFinished.count(), 1);
        // The queued tail completes without running a render. Every running
        // response has finished before shutdown returns, while Qt is alive.
        std::unique_ptr<QQuickTextureFactory> tail(responses.back()->textureFactory());
        QVERIFY(!tail || tail->textureSize().isEmpty());
        QVERIFY(provider.requestImage(deck.renderId(0), nullptr, QSize(1920, 1080)).isNull());
        std::unique_ptr<QQuickImageResponse> late(provider.requestImageResponse(deck.renderId(0), QSize(340, 192)));
        std::unique_ptr<QQuickTextureFactory> lateTexture(late->textureFactory());
        QVERIFY(!lateTexture || lateTexture->textureSize().isEmpty());
        provider.shutdown(); // Idempotent for aboutToQuit, scope guard, and destructor.
    }
    void navigationLatency() {
        if (!qEnvironmentVariableIsSet("HYPE_BENCHMARK"))
            QSKIP("Set HYPE_BENCHMARK=1 for navigation timings");
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        Deck d;
        QString path = qEnvironmentVariable("HYPE_BENCHMARK_DECK", QFINDTESTDATA("../trials/rails-world-2025/presentation.md"));
        QVERIFY(d.loadPath(path));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &d);
        engine.addImageProvider("slides", new Thumbnails(&d));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        auto preview = window->findChild<QQuickItem *>("slidePreview");
        auto quick = window->findChild<QQuickItem *>("slideQuickPreview");
        QVERIFY(window && preview && quick);
        QTest::qWait(1500);
        // Walk forward at a reading pace, then at a held-key pace; report how long the
        // GUI thread is blocked by a selection and how long until the stage shows it.
        // A heartbeat exposes GUI-thread stalls that land between selections, such as the
        // deferred prefetch scan.
        QVector<qint64> gaps;
        QElapsedTimer beat;
        QTimer heart;
        heart.setInterval(2);
        connect(&heart, &QTimer::timeout, &heart, [&] { gaps.append(beat.restart()); });
        for (int pause : {400, 30}) {
            QVector<qint64> blocked, shown, first;
            gaps.clear();
            beat.start();
            heart.start();
            const int last = qMin(d.count() - 1, 60);
            d.select(0);
            QTest::qWait(1500);
            for (int i = 1; i <= last; ++i) {
                QElapsedTimer timer;
                timer.start();
                d.select(i);
                QCoreApplication::processEvents();
                blocked.append(timer.elapsed());
                const QString expected = "image://slides/" + d.renderId(i);
                qint64 firstShown = -1;
                while (timer.elapsed() < 3000) {
                    const bool sharp = preview->property("status").toInt() == 1 &&
                                       preview->property("source").toString().startsWith(expected);
                    if (firstShown < 0 && (sharp || quick->isVisible()))
                        firstShown = timer.elapsed();
                    if (sharp)
                        break;
                    QTest::qWait(1);
                }
                first.append(firstShown < 0 ? timer.elapsed() : firstShown);
                shown.append(timer.elapsed());
                QTest::qWait(pause);
            }
            auto describe = [](QVector<qint64> values) {
                std::sort(values.begin(), values.end());
                qint64 total = 0;
                for (auto v : values) total += v;
                return QString("mean %1 ms, p90 %2 ms, max %3 ms").arg(total / values.size())
                    .arg(values[values.size() * 9 / 10]).arg(values.last());
            };
            qInfo().noquote() << "Pause" << pause << "ms  blocked:" << describe(blocked);
            qInfo().noquote() << "Pause" << pause << "ms  first:  " << describe(first);
            qInfo().noquote() << "Pause" << pause << "ms  sharp:  " << describe(shown);
            heart.stop();
            qInfo().noquote() << "Pause" << pause << "ms  stalls: " << describe(gaps);
        }
        window->setProperty("allowClose", true);
        window->close();
    }
    void trialRenderingPerformance() {
        if (!qEnvironmentVariableIsSet("HYPE_BENCHMARK"))
            QSKIP("Set HYPE_BENCHMARK=1 for trial rendering timings");
        Deck d;
        QString path = qEnvironmentVariable("HYPE_BENCHMARK_DECK", QFINDTESTDATA("../trials/rails-world-2025/presentation.md"));
        QVERIFY(d.loadPath(path));
        Thumbnails provider(&d);
        QVector<QString> ids;
        for (int i = 0; i < d.count(); ++i)
            ids.append(d.renderId(i));
        for (int pass = 0; pass < 2; ++pass) {
            QElapsedTimer timer;
            timer.start();
            for (const QString &id : ids) {
                QSize size;
                QVERIFY(!provider.requestImage(id, &size, QSize(340, 192)).isNull());
            }
            qInfo() << (pass ? "Cached" : "Cold") << ids.size()
                    << "trial thumbnails:" << timer.elapsed() << "ms";
        }
        for (int pass = 0; pass < 2; ++pass) {
            QElapsedTimer timer;
            timer.start();
            for (const QString &id : ids.mid(0, 10))
                QVERIFY(!provider.requestImage(id, nullptr, QSize(1920, 1080)).isNull());
            qInfo() << (pass ? "Cached" : "Cold") << "first ten full previews:" << timer.elapsed() << "ms";
        }
        // Cold full-size cost per slide, slowest first: what a jump to an unprefetched slide pays.
        QVector<QPair<qint64, int>> costs;
        for (int i = 10; i < d.count(); ++i) {
            QElapsedTimer timer;
            timer.start();
            provider.requestImage(ids[i], nullptr, QSize(1920, 1080));
            costs.append({timer.elapsed(), i});
        }
        std::sort(costs.begin(), costs.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        qint64 total = 0;
        for (const auto &cost : costs) total += cost.first;
        qInfo() << "Cold full previews for the rest:" << total << "ms, mean" << (costs.isEmpty() ? 0 : total / costs.size()) << "ms";
        for (const auto &cost : costs.mid(0, 8)) {
            const auto media = parseMedia(d.slide(cost.second), d.baseDir());
            qInfo().noquote() << QString("  slide %1: %2 ms  %3 %4 %5%6").arg(cost.second + 1).arg(cost.first)
                .arg(media.file, QImageReader(media.path).size().isValid() ? QString("%1x%2").arg(QImageReader(media.path).size().width()).arg(QImageReader(media.path).size().height()) : QString("-"),
                     media.span ? "span" : "fit", media.text.trimmed().isEmpty() ? "" : " +text");
        }
        int nextImage = 10;
        while (nextImage < d.count()) {
            const auto media = parseMedia(d.slide(nextImage), d.baseDir());
            if (!media.file.isEmpty() && !media.video) break;
            ++nextImage;
        }
        QVERIFY(nextImage < d.count());
        d.select(nextImage - 1);
        QTest::qWait(750);
        QElapsedTimer warmed;
        warmed.start();
        QVERIFY(!provider.requestImage(d.renderId(nextImage), nullptr, QSize(1920, 1080)).isNull());
        qInfo() << "Prefetched next full preview:" << warmed.nsecsElapsed() / 1000 << "microseconds";
    }
    void previewCacheSharedAcrossWorkers() {
        Deck deck;
        deck.editSource("# Shared preview cache");
        Thumbnails provider(&deck);
        const auto id = deck.renderId(0);
        const auto first = provider.requestImage(id, nullptr, QSize(340, 192));
        auto future = QtConcurrent::run([&] { return provider.requestImage(id, nullptr, QSize(340, 192)); });
        future.waitForFinished();
        QCOMPARE(future.result().cacheKey(), first.cacheKey());
        deck.editSource("# Updated preview cache");
        const auto updated = provider.requestImage(deck.renderId(0), nullptr, QSize(340, 192));
        QVERIFY(updated != first);
        // Larger rendering should retain the cached sidebar image.
        QVERIFY(!provider.requestImage(id, nullptr, QSize(1920, 1080)).isNull());
        QCOMPARE(provider.requestImage(id, nullptr, QSize(340, 192)).cacheKey(), first.cacheKey());
    }
    void rapidImageNavigation() {
        if (!qEnvironmentVariableIsSet("HYPE_GUI_TESTS"))
            QSKIP("Set HYPE_GUI_TESTS=1 with local multimedia access");
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        QString source;
        for (int i = 0; i < 18; ++i) {
            QImage image(1920, 1080, QImage::Format_RGB32);
            image.fill(QColor::fromHsv(i * 19, 230, 210));
            QVERIFY(image.save(QString("%1/images/%2.png").arg(tmp.path()).arg(i)));
            if (i) source += "\n---\n";
            source += QString("![span](%1.png)").arg(i);
        }
        write(tmp.path() + "/talk.md", source);
        Deck deck;
        QVERIFY(deck.loadPath(tmp.path() + "/talk.md"));
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
        qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("deck", &deck);
        engine.addImageProvider("slides", new Thumbnails(&deck));
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects()[0]);
        auto preview = window->findChild<QQuickItem *>("slidePreview");
        QVERIFY(preview);
        for (int i = 0; i < 40; ++i) {
            deck.select((i * 7) % 18);
            QTest::qWait(5);
        }
        deck.select(17);
        QTRY_COMPARE_WITH_TIMEOUT(preview->property("status").toInt(), 1, 5000);
        auto capture = preview->grabToImage();
        QTRY_VERIFY(!capture->image().isNull());
        QCOMPARE(capture->image().pixelColor(capture->image().width() / 2, capture->image().height() / 2),
                 QColor::fromRgb(QColor::fromHsv(17 * 19, 230, 210).rgb()));
        // Let any earlier requests finish: they must not replace the chosen slide.
        QTest::qWait(250);
        capture = preview->grabToImage();
        QTRY_VERIFY(!capture->image().isNull());
        QCOMPARE(capture->image().pixelColor(capture->image().width() / 2, capture->image().height() / 2),
                 QColor::fromRgb(QColor::fromHsv(17 * 19, 230, 210).rgb()));
    }
    void imageTextOverlaysEveryLayout() {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        QImage portrait(80, 160, QImage::Format_RGB32);
        portrait.fill(Qt::blue);
        QVERIFY(portrait.save(tmp.path() + "/images/portrait.png"));
        Deck deck;
        QVERIFY(deck.savePath(tmp.path() + "/talk.md"));
        Thumbnails provider(&deck);
        for (const QString &layout : {QString("fit"), QString("background=blur"),
                                     QString("fit background=auto")}) {
            const QString mediaSource = "![" + layout + "](portrait.png)";
            deck.editSlide(mediaSource + "\n\n# Headline");
            const auto media = parseMedia(deck.slideSource(), deck.baseDir());
            QVERIFY(!media.span);
            QCOMPARE(media.overlay, 0.25);
            QCOMPARE(mediaRect(media), mediaRect(parseMedia(mediaSource, deck.baseDir())));
            const auto rendered = provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180));
            int top = 180, bottom = 0;
            for (int y = 0; y < rendered.height(); ++y)
                for (int x = 0; x < rendered.width(); ++x) {
                    const auto color = rendered.pixelColor(x, y);
                    if (color.red() > 220 && color.green() > 220 && color.blue() > 220) {
                        top = qMin(top, y);
                        bottom = qMax(bottom, y);
                    }
                }
            QVERIFY(top > 50 && top < bottom && bottom < 130); // Centered over the picture.
            const auto overlay = provider.requestImage(deck.renderId(0) + "/overlay", nullptr, QSize(320, 180));
            QCOMPARE(overlay.pixelColor(0, 0).alpha(), 64);
        }
        deck.editSlide("![background=blur](portrait.png)\n\n# Headline");
        QVERIFY(deck.exportPdf(tmp.path() + "/talk.pdf"));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(tmp.path() + "/talk.pdf"), QPdfDocument::Error::None);
        const auto page = pdf.render(0, QSize(320, 180));
        const auto background = page.pixelColor(0, 0);
        QCOMPARE(background.red(), 0);
        QCOMPARE(background.green(), 0);
        QVERIFY(qAbs(background.blue() - 191) <= 1); // PDF transparency rounding.
        deck.editSlide("![fit overlay=0](portrait.png)\n\n# Headline");
        QCOMPARE(parseMedia(deck.slideSource(), deck.baseDir()).overlay, 0.0);
    }
    void picturesSoftenOnlyBehindText() {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        QImage picture(1920, 1080, QImage::Format_RGB32);
        picture.fill(Qt::black);
        {
            QPainter painter(&picture);
            painter.fillRect(960, 0, 960, 1080, Qt::white);
        }
        const QString path = tmp.path() + "/images/picture.png";
        QVERIFY(picture.save(path));
        Deck deck;
        QVERIFY(deck.savePath(tmp.path() + "/talk.md"));
        Thumbnails provider(&deck);
        for (const QString &layout : {QString("span"), QString("fit"), QString("fit background=blur")}) {
            const QString source = "![" + layout + " overlay=0](picture.png)";
            deck.editSlide(source);
            const auto sharp = provider.requestImage(deck.renderId(0), nullptr, QSize(1920, 1080));
            QVERIFY(sharp.pixelColor(958, 300).red() < 5);
            deck.editSlide(source + "\n# A sharp headline");
            const auto soft = provider.requestImage(deck.renderId(0), nullptr, QSize(1920, 1080));
            const int edge = soft.pixelColor(958, 300).red();
            QVERIFY(edge > 10 && edge < 240); // Only a narrow transition at the picture's edge.
            QCOMPARE(soft.pixelColor(940, 300), QColor(Qt::black));
            QCOMPARE(soft.pixelColor(980, 300), QColor(Qt::white));
        }
        QVERIFY(deck.exportPdf(tmp.path() + "/talk.pdf"));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(tmp.path() + "/talk.pdf"), QPdfDocument::Error::None);
        const auto exported = pdf.render(0, QSize(3840, 2160));
        const int edge = exported.pixelColor(1916, 600).red();
        QVERIFY(edge > 10 && edge < 240);
        QCOMPARE(QImage(path), picture); // Rendering never changes the source asset.
        const auto cached = softenedImage(picture, picture.size());
        const auto reused = QtConcurrent::run([&] { return softenedImage(picture, picture.size()); }).result();
        QCOMPARE(cached.cacheKey(), reused.cacheKey());
    }
    void blurredImageBackground() {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        QImage portrait(80, 160, QImage::Format_ARGB32_Premultiplied);
        portrait.fill(Qt::red);
        {
            QPainter painter(&portrait);
            painter.fillRect(0, 80, 80, 80, Qt::blue);
        }
        QVERIFY(portrait.save(tmp.path() + "/images/portrait.png"));
        write(tmp.path() + "/talk.md", "![fit background=theme](portrait.png)\n");
        Deck deck;
        QVERIFY(deck.loadPath(tmp.path() + "/talk.md"));
        const QString original = deck.source();
        deck.setMediaBackground("blur");
        const auto media = parseMedia(deck.slideSource(), deck.baseDir());
        QVERIFY(media.error.isEmpty());
        QCOMPARE(media.background, QString("blur"));
        QVERIFY(!media.span);
        Thumbnails provider(&deck);
        const auto rendered = provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180));
        QCOMPARE(rendered.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(rendered.pixelColor(0, 179), QColor(Qt::blue));
        const auto transition = rendered.pixelColor(0, 89);
        QVERIFY(transition.red() > 80 && transition.blue() > 80);
        // Only the stretched background is blurred; the foreground remains sharp.
        QCOMPARE(rendered.pixelColor(160, 80), QColor(Qt::red));
        QCOMPARE(rendered.pixelColor(160, 100), QColor(Qt::blue));
        const auto background = provider.requestImage(deck.renderId(0) + "/background", nullptr,
                                                       QSize(320, 180));
        QVERIFY(background.pixelColor(160, 80).blue() > 20);
        QImage large(3840, 2160, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter painter(&large);
            paintSlide(&painter, large.rect(), deck.slideSource(), deck.baseDir(), deck.palette());
        }
        QCOMPARE(large.pixelColor(1920, 960), QColor(Qt::red));
        QVERIFY(large.pixelColor(0, 1074).blue() > 80);
        deck.matchImageBackground(false);
        QCOMPARE(parseMedia(deck.slideSource(), deck.baseDir()).background, QString("theme"));
        deck.undo();
        QCOMPARE(parseMedia(deck.slideSource(), deck.baseDir()).background, QString("blur"));
        deck.undo();
        QCOMPARE(deck.source(), original);
        // Transparent artwork should reveal the theme, without black blur fringes.
        portrait.fill(Qt::transparent);
        QVERIFY(portrait.save(tmp.path() + "/images/transparent.png"));
        deck.editSlide("![fit background=blur](transparent.png)");
        QCOMPARE(provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180)).pixelColor(0, 0),
                 deck.background());
    }
    void blurredVideoBackground() {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        QVERIFY(QDir().mkpath(tmp.path() + "/videos"));
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i",
            "color=c=blue:s=90x160:d=0.2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
            tmp.path() + "/videos/demo.mp4"});
        QVERIFY(ffmpeg.waitForFinished(30000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        QImage poster(90, 160, QImage::Format_RGB32);
        poster.fill(Qt::green);
        QVERIFY(poster.save(tmp.path() + "/images/poster.png"));
        const QString example = "```markdown\n![span](example.mp4)\n```\n\n";
        write(tmp.path() + "/talk.md", example +
            "![span loop muted autoplay=false poster=poster.png alt=\"A span of time\"](demo.mp4)");
        Deck deck;
        QVERIFY(deck.loadPath(tmp.path() + "/talk.md"));
        const QString original = deck.source();
        deck.setMediaBackground("blur");
        QVERIFY(deck.slideSource().startsWith(example));
        QVERIFY(deck.slideSource().contains("alt=\"A span of time\""));
        auto media = parseMedia(deck.slideSource(), deck.baseDir());
        QVERIFY(media.error.isEmpty());
        QVERIFY(media.video && !media.span && media.loop && media.muted && !media.autoplay);
        QCOMPARE(media.background, QString("blur"));
        QCOMPARE(media.poster, tmp.path() + "/images/poster.png");
        deck.undo();
        QCOMPARE(deck.source(), original);

        // A custom poster remains in the foreground; the blur comes from the video.
        deck.editSlide("![fit background=blur poster=poster.png](demo.mp4)");
        Thumbnails provider(&deck);
        auto rendered = provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180));
        const QColor blue = rendered.pixelColor(0, 0);
        QVERIFY(blue.blue() > 245 && blue.red() < 10 && blue.green() < 10);
        QCOMPARE(rendered.pixelColor(160, 90), QColor(Qt::green));
        const QString firstFrame = ensurePoster(media.path, deck.baseDir());
        QVERIFY(!firstFrame.isEmpty());
        const auto modified = QFileInfo(firstFrame).lastModified();
        // Rendering again reuses the extracted first frame, independent of playback.
        QCOMPARE(ensurePoster(media.path, deck.baseDir()), firstFrame);
        QCOMPARE(QFileInfo(firstFrame).lastModified(), modified);

        QVERIFY(deck.exportPdf(tmp.path() + "/talk.pdf"));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(tmp.path() + "/talk.pdf"), QPdfDocument::Error::None);
        const auto page = pdf.render(0, QSize(320, 180));
        QVERIFY(page.pixelColor(0, 0).blue() > 245);
        QCOMPARE(page.pixelColor(160, 90), QColor(Qt::green));
        deck.editSlide("![span loop poster=poster.png](demo.mp4)");
        deck.matchImageBackground(true);
        media = parseMedia(deck.slideSource(), deck.baseDir());
        QVERIFY(media.video && !media.span && media.loop);
        QCOMPARE(media.background, QString("auto"));
        rendered = provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180));
        QVERIFY(rendered.pixelColor(0, 0).blue() > 245);
        QVERIFY(rendered.pixelColor(0, 0).green() < 10);
        QCOMPARE(rendered.pixelColor(160, 90), QColor(Qt::green));
        QVERIFY(deck.exportPdf(tmp.path() + "/matched.pdf"));
        QPdfDocument matched;
        QCOMPARE(matched.load(tmp.path() + "/matched.pdf"), QPdfDocument::Error::None);
        QVERIFY(matched.render(0, QSize(320, 180)).pixelColor(0, 0).blue() > 245);
        deck.editSlide("![fit background=blur](demo.mp4)");
        rendered = provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180));
        QVERIFY(rendered.pixelColor(0, 0).blue() > 245);
        QVERIFY(rendered.pixelColor(160, 90).blue() > 245);
        deck.setMediaBackground("theme");
        rendered = provider.requestImage(deck.renderId(0), nullptr, QSize(320, 180));
        QCOMPARE(rendered.pixelColor(0, 0), deck.background());
    }
    void backgroundAndCache() {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.path() + "/images");
        QImage artwork(100, 100, QImage::Format_ARGB32);
        artwork.fill(QColor("#f8f5f2"));
        {
            QPainter painter(&artwork);
            painter.fillRect(QRect(5, 5, 90, 90), Qt::black);
        }
        QVERIFY(artwork.save(tmp.path() + "/images/art.png"));
        write(tmp.path() + "/talk.md", "![fit](art.png)");
        Deck d;
        QVERIFY(d.loadPath(tmp.path() + "/talk.md"));
        QString original = d.source();
        Thumbnails defaults(&d);
        QSize defaultSize;
        QCOMPARE(
            defaults.requestImage(d.renderId(0), &defaultSize, QSize(320, 180)).pixelColor(0, 0),
            QColor("#f8f5f2"));
        d.editSlide("![](art.png)");
        QCOMPARE(
            defaults.requestImage(d.renderId(0), &defaultSize, QSize(320, 180)).pixelColor(0, 0),
            QColor("#f8f5f2"));
        d.undo();
        d.matchImageBackground(true);
        QCOMPARE(parseMedia(d.slideSource(), d.baseDir()).background, QString("auto"));
        Thumbnails provider(&d);
        QSize size;
        QString id = d.renderId(0);
        auto image = provider.requestImage(id, &size, QSize(320, 180));
        QCOMPARE(image.pixelColor(0, 0), QColor("#f8f5f2"));
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 1000; ++i)
            QCOMPARE(provider.requestImage(id, &size, QSize(320, 180)), image);
        qInfo() << "1000 cached slide requests:" << timer.elapsed() << "ms";
        d.matchImageBackground(false);
        QCOMPARE(provider.requestImage(d.renderId(0), &size, QSize(320, 180)).pixelColor(0, 0),
                 d.background());
        d.undo();
        d.undo();
        QCOMPARE(d.source(), original);
        artwork.fill(Qt::transparent);
        QVERIFY(artwork.save(tmp.path() + "/images/art.png"));
        d.matchImageBackground(true);
        QCOMPARE(provider.requestImage(d.renderId(0), &size, QSize(320, 180)).pixelColor(0, 0),
                 d.background());
        QSignalSpy resets(&d, &QAbstractItemModel::modelReset);
        d.editSlide("# Updated");
        QCOMPARE(resets.count(), 0);
    }
    void syntaxColors() {
        Deck deck;
        QTextDocument doc;
        doc.setMarkdown("# Ruby\n\n```ruby\nclass Post\n  # café\n  puts "
                        "\"héllo\"\nend\n```\n\nPlain text\n\n```javascript\nconst n = 42;\n```\n");
        QString original = doc.toPlainText();
        highlightCode(doc, deck.palette());
        QCOMPARE(doc.toPlainText(), original);
        auto colorAt = [&](QString token) {
            QTextCursor cursor(&doc);
            cursor.setPosition(original.indexOf(token));
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            return cursor.charFormat().foreground().color();
        };
        QCOMPARE(colorAt("class"), QColor(deck.palette()["magenta"].toString()));
        QCOMPARE(colorAt("héllo"), QColor(deck.palette()["green"].toString()));
        QCOMPARE(colorAt("const"), QColor(deck.palette()["magenta"].toString()));
        QCOMPARE(colorAt("42"), QColor(deck.palette()["red"].toString()));
        auto palette = deck.palette();
        palette["green"] = "#123456";
        highlightCode(doc, palette);
        QCOMPARE(colorAt("héllo"), QColor("#123456"));
        QTextDocument plain;
        plain.setMarkdown("```unknownlanguage\nclass Post\n```\n\n```\nclass Plain\n```");
        original = plain.toPlainText();
        highlightCode(plain, palette);
        QCOMPARE(plain.toPlainText(), original);
    }
    void headlineFormattingStaysLocal() {
        Deck deck;
        const QStringList markedWords{"_Ruby_", "*Ruby*", "**Ruby**", "~~Ruby~~", "`Ruby`"};
        for (int level : {1, 2, 3, 4, 5, 6}) {
            for (const auto &word : markedWords) {
                QTextDocument doc;
                layoutSlideText(doc, QString(level, '#') + " No more " + word + " programmers,\n" +
                    QString(level, '#') + " just programmers\n\nPlain text", deck.palette(), 60, 1660, true, false);
                auto formatAt = [&](int position) {
                    QTextCursor cursor(&doc);
                    cursor.setPosition(position);
                    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                    return cursor.charFormat();
                };
                const auto plain = doc.toPlainText();
                const int firstHeadingEnd = plain.indexOf('\n');
                const int ruby = plain.indexOf("Ruby");
                const int expectedSize = qRound(60 * (level == 1 ? 1.8 : 1.25));
                for (int i = 0; i < firstHeadingEnd; ++i) {
                    const auto format = formatAt(i);
                    QVERIFY2(format.fontWeight() >= QFont::Bold, qPrintable(word + " at " + QString::number(i)));
                    QCOMPARE(format.intProperty(QTextFormat::FontPixelSize), expectedSize);
                    QCOMPARE(format.fontUnderline(), word == "_Ruby_" && i >= ruby && i < ruby + 4);
                    QCOMPARE(format.fontItalic(), word == "*Ruby*" && i >= ruby && i < ruby + 4);
                    QCOMPARE(format.fontStrikeOut(), word == "~~Ruby~~" && i >= ruby && i < ruby + 4);
                }
                const auto nextHeading = formatAt(plain.indexOf("just"));
                QCOMPARE(nextHeading.intProperty(QTextFormat::FontPixelSize), expectedSize);
                QVERIFY(nextHeading.fontWeight() >= QFont::Bold);
                // Qt's relative heading size can override FontPixelSize during
                // layout. Check the actual fonts used to draw each glyph run.
                doc.documentLayout()->documentSize();
                const auto referenceBlock = doc.findBlock(plain.indexOf("just"));
                const auto referenceRuns = referenceBlock.layout()->glyphRuns(0, referenceBlock.length() - 1);
                QVERIFY2(!referenceRuns.isEmpty(), qPrintable(QString("Heading %1, %2, block [%3], position %4, text [%5]")
                    .arg(level).arg(word, referenceBlock.text()).arg(plain.indexOf("just")).arg(plain)));
                const auto reference = referenceRuns.first().rawFont();
                for (const auto &run : doc.begin().layout()->glyphRuns(0, doc.begin().length() - 1)) {
                    QCOMPARE(run.rawFont().pixelSize(), reference.pixelSize());
                    QCOMPARE(run.rawFont().weight(), reference.weight());
                }
                const auto body = formatAt(plain.indexOf("Plain"));
                QCOMPARE(body.fontWeight(), QFont::Normal);
                QCOMPARE(body.intProperty(QTextFormat::FontPixelSize), 60);
            }
        }
    }
    void rustSyntaxColors() {
        Deck deck;
        for (const QString &language : {QString("rust"), QString("rs"), QString("Rust")}) {
            QTextDocument doc;
            doc.setMarkdown("```" + language + "\nfn main() {\n  // café\n"
                            "  let answer: u32 = 42;\n  println!(\"héllo\");\n}\n```\n");
            const QString original = doc.toPlainText();
            highlightCode(doc, deck.palette());
            QCOMPARE(doc.toPlainText(), original);
            auto colorAt = [&](const QString &token) {
                QTextCursor cursor(&doc);
                cursor.setPosition(original.indexOf(token));
                cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                return cursor.charFormat().foreground().color();
            };
            QCOMPARE(colorAt("fn"), QColor(deck.palette()["magenta"].toString()));
            QCOMPARE(colorAt("let"), QColor(deck.palette()["magenta"].toString()));
            QCOMPARE(colorAt("u32"), QColor(deck.palette()["yellow"].toString()));
            QCOMPARE(colorAt("42"), QColor(deck.palette()["red"].toString()));
            QCOMPARE(colorAt("héllo"), QColor(deck.palette()["green"].toString()));
            QCOMPARE(colorAt("café"), QColor(deck.palette()["dark_foreground"].toString()));
        }
    }
    void plainLineBreaks() {
        Deck deck;
        auto render = [&](const QString &source) {
            QImage image(960, 540, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&image);
            paintSlide(&painter, image.rect(), source, "/tmp", deck.palette());
            return image;
        };
        QString cities = "San Clarita\nChicago\nVirginia\nCopenhagen\nAmsterdam\nSingapore\nSydney";
        QString explicitBreaks = cities;
        explicitBreaks.replace("\n", "\\\n");
        QCOMPARE(render(cities), render(explicitBreaks));
        QString crlf = cities;
        crlf.replace("\n", "\r\n");
        QCOMPARE(render(crlf), render(explicitBreaks));
        QString cr = cities;
        cr.replace("\n", "\r");
        QCOMPARE(render(cr), render(explicitBreaks));
        QCOMPARE(render("`one`\n`two`"), render("`one`\\\n`two`"));
    }
    void pdfPreserves4kDetailAndReusesImages() {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.path() + "/images"));
        QImage detail(3840, 2160, QImage::Format_RGB32);
        for (int y = 0; y < detail.height(); ++y) {
            auto row = reinterpret_cast<QRgb *>(detail.scanLine(y));
            for (int x = 0; x < detail.width(); ++x)
                row[x] = x % 2 ? qRgb(255, 0, 0) : qRgb(0, 0, 255);
        }
        QVERIFY(detail.save(tmp.path() + "/images/detail.png"));
        write(tmp.path() + "/talk.md", "![span](detail.png)\n");
        Deck deck;
        QVERIFY(deck.loadPath(tmp.path() + "/talk.md"));
        QVERIFY(deck.exportPdf(tmp.path() + "/one.pdf"));
        deck.editSource("![span](detail.png)\n---\n![span](detail.png)\n");
        QVERIFY(deck.exportPdf(tmp.path() + "/two.pdf"));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(tmp.path() + "/two.pdf"), QPdfDocument::Error::None);
        QCOMPARE(pdf.pageCount(), 2);
        const auto rendered = pdf.render(0, QSize(3840, 2160));
        QVERIFY(!rendered.isNull());
        // One-pixel red/blue stripes catch both the old 2560px cap and JPEG loss.
        for (int x = 100; x < 300; ++x)
            QCOMPARE(rendered.pixelColor(x, 100), detail.pixelColor(x, 100));
        QVERIFY(QFileInfo(tmp.path() + "/two.pdf").size() <
                QFileInfo(tmp.path() + "/one.pdf").size() + 5000);
        QFile file(tmp.path() + "/two.pdf");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(!bytes.contains("/DCTDecode"));
        QVERIFY(bytes.contains("/Width 3840"));
        QVERIFY(bytes.contains("/Height 2160"));
        // A portrait in Span must embed only its visible center, not hidden pixels.
        QImage portrait(2160, 3840, QImage::Format_RGB32);
        portrait.fill(Qt::blue);
        {
            QPainter painter(&portrait);
            painter.fillRect(0, 1300, 2160, 1240, Qt::red);
        }
        QVERIFY(portrait.save(tmp.path() + "/images/portrait.png"));
        deck.editSource("![span](portrait.png)\n");
        QVERIFY(deck.exportPdf(tmp.path() + "/crop.pdf"));
        QFile cropped(tmp.path() + "/crop.pdf");
        QVERIFY(cropped.open(QIODevice::ReadOnly));
        const auto croppedBytes = cropped.readAll();
        QVERIFY(croppedBytes.contains("/Width 2160"));
        QVERIFY(croppedBytes.contains("/Height 1215"));
        QPdfDocument croppedPdf;
        QCOMPARE(croppedPdf.load(tmp.path() + "/crop.pdf"), QPdfDocument::Error::None);
        const auto croppedPage = croppedPdf.render(0, QSize(384, 216));
        QCOMPARE(croppedPage.pixelColor(190, 1), QColor(Qt::red));
        QCOMPARE(croppedPage.pixelColor(190, 214), QColor(Qt::red));
    }
    void renderAndPdf() {
        QTemporaryDir tmp;
        Deck d;
        d.editSource("# Theme\n\n---\n\n> A quote with **emphasis**.\n\n— "
                     "Author\n\n---\n\n```ruby\nclass Post\n  belongs_to "
                     ":author\nend\n```\n");
        QVERIFY(d.renderImages(tmp.path() + "/render"));
        QImage image(tmp.path() + "/render/slide-001.png");
        QCOMPARE(image.size(), QSize(1920, 1080));
        QCOMPARE(image.pixelColor(0, 0), d.background());
        QVERIFY(d.exportPdf(tmp.path() + "/talk.pdf"));
        QPdfDocument pdf;
        QCOMPARE(pdf.load(tmp.path() + "/talk.pdf"), QPdfDocument::Error::None);
        QCOMPARE(pdf.pageCount(), 3);
    }
    void missingMediaDoesNotOverwriteExport() {
        QTemporaryDir tmp;
        QString path = tmp.path() + "/talk.pdf";
        write(path, "existing");
        Deck d;
        d.editSource("![](does-not-exist.png)");
        QVERIFY(!d.exportPdf(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("existing"));
    }
};
QTEST_MAIN(HypeTests)
#include "tests.moc"
