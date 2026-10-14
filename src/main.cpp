#include "apptheme.h"
#include "cli.h"
#include "deck.h"
#include "renderer.h"
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTimer>
#include <cstdio>
// The desktop's interface font, e.g. "Adwaita Sans 11", which the gtk3 platform
// theme used to supply. Without a settings portal Qt's default font stays.
static void adoptDesktopFont() {
    auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.Settings", "ReadOne");
    call.setArguments({"org.gnome.desktop.interface", "font-name"});
    const auto reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return;
    const QString name = reply.arguments().first().value<QDBusVariant>().variant().toString();
    const int space = name.lastIndexOf(' ');
    const double size = name.mid(space + 1).toDouble();
    if (space <= 0 || size <= 0) return;
    QFont font(name.left(space));
    font.setPointSizeF(size);
    QGuiApplication::setFont(font);
}
int main(int argc, char **argv) {
    // Hype themes itself. Qt's gtk3 platform theme only adds a use-after-free
    // inside GTK when the desktop theme changes under a running editor.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    // Commands, exports and help draw no window, so they must not need a display,
    // even where the desktop exports QT_QPA_PLATFORM=wayland.
    // Bare hype prints help, as a command line tool should; launchers say hype open.
    const bool command = argc == 1 || isCliCommand(argv[1]);
    bool windowless = command;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        for (const char *option : {"--pdf", "--pptx", "--render", "--help", "--version"})
            windowless = windowless || argument.startsWith(option);
        windowless = windowless || argument == "-h" || argument == "-v";
    }
    if (windowless)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setApplicationName("hype");
    app.setApplicationVersion("0.4.3");
    app.setDesktopFileName(qEnvironmentVariable("HYPE_DESKTOP_FILE", "hype"));
    if (command)
        return runCli(app.arguments());
    QCommandLineParser args;
    args.setApplicationDescription("Simple Markdown presentations with a visual slide editor.\n\n" + cliSummary());
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument("presentation", "Markdown presentation");
    args.addOption({"pdf", "Export PDF and exit", "file"});
    args.addOption({"pptx", "Export rendered PowerPoint and exit", "file"});
    args.addOption({"render", "Render slide PNGs and manifest and exit", "directory"});
    args.addOption({"theme", "Apply installed theme", "name"});
    args.addOption({"save", "Save changes (for theme snapshots)"});
    args.addOption({"slide", "Select a slide (1-based)", "number"});
    args.addOption({"markdown", "Start in full-document Markdown mode"});
    args.addOption({"overview", "Start in slide overview mode"});
    args.addOption({"screenshot", "Save editor screenshot and exit", "file"});
    QCommandLineOption snapshotOption("export-snapshot", "Internal export snapshot", "file");
    snapshotOption.setFlags(QCommandLineOption::HiddenFromHelp);
    args.addOption(snapshotOption);
    QStringList arguments = app.arguments();
    if (arguments.value(1) == "open")
        arguments.removeAt(1);
    args.process(arguments);
    Deck deck;
    const bool exportWorker = args.isSet(snapshotOption);
    auto report = [](const QJsonObject &event) {
        const auto line = QJsonDocument(event).toJson(QJsonDocument::Compact);
        fprintf(stdout, "%s\n", line.constData());
        fflush(stdout);
    };
    if (exportWorker) {
        if (!args.isSet("pdf") && !args.isSet("pptx")) return 1;
        if (!deck.loadExportSnapshot(args.value(snapshotOption))) {
            report({{"error", deck.status()}});
            return 1;
        }
        QObject::connect(&deck, &Deck::exportAdvanced, &app, [report](double progress, const QString &message) {
            report({{"progress", progress}, {"message", message}});
        });
    }
    auto positional = args.positionalArguments();
    const bool exporting = args.isSet("pdf") || args.isSet("pptx") || args.isSet("render");
    if (exporting && positional.isEmpty() && !exportWorker) {
        fprintf(stderr, "Name a Markdown presentation to export.\n");
        return 1;
    }
    if (!positional.isEmpty() && !deck.loadPath(positional[0], !exporting)) {
        fprintf(stderr, "%s\n", qPrintable(deck.status()));
        return 1;
    }
    if (positional.isEmpty() && !exportWorker)
        deck.reopenLastPresentation();
    if (args.isSet("theme"))
        deck.chooseTheme(args.value("theme"));
    if (args.isSet("save"))
        deck.save();
    if (args.isSet("slide"))
        deck.select(args.value("slide").toInt() - 1);
    bool success = true, headless = false;
    for (const QString &option : {QString("pdf"), QString("pptx"), QString("render")})
        if (args.isSet(option)) {
            headless = true;
            success = success && (option == "pdf"    ? deck.exportPdf(args.value(option))
                                  : option == "pptx" ? deck.exportPptx(args.value(option))
                                                     : deck.renderImages(args.value(option)));
        }
    if (headless) {
        if (exportWorker) {
            if (success) report({{"progress", 1.0}, {"message", deck.status()}});
            else report({{"error", deck.status()}});
            return success ? 0 : 1;
        }
        fprintf(success ? stdout : stderr, "%s\n", qPrintable(deck.status()));
        return success ? 0 : 1;
    }
    deck.enableAutosave();
    adoptDesktopFont();
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
    qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("deck", &deck);
    QPointer<Thumbnails> thumbnails = new Thumbnails(&deck);
    engine.addImageProvider("slides", thumbnails);
    auto drainRenders = [thumbnails] {
        if (thumbnails)
            thumbnails->shutdown();
        // Clipboard image compression can also decode SVG through Qt GUI.
        QThreadPool::globalInstance()->waitForDone();
    };
    // The engine is not the final owner of an async image provider. Drain while
    // QGuiApplication's fonts, platform integration and GPU resources still exist.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, drainRenders);
    const auto renderShutdown = qScopeGuard(drainRenders);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (args.isSet("markdown"))
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openMarkdown");
    if (args.isSet("overview"))
        QMetaObject::invokeMethod(engine.rootObjects().first(), "setMode", Q_ARG(QVariant, "overview"));
    if (args.isSet("screenshot")) {
        QTimer::singleShot(1800, &app, [&] {
            auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            bool ok = window && window->grabWindow().save(args.value("screenshot"));
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
