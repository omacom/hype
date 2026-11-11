#include "filedialog.h"
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QUrl>
#include <QUuid>

namespace {
const QString service = "org.freedesktop.portal.Desktop";
const QString requestInterface = "org.freedesktop.portal.Request";
struct FilterRule { uint type; QString pattern; };
using FilterRules = QList<FilterRule>;
struct Filter { QString label; FilterRules rules; };
using Filters = QList<Filter>;
QDBusArgument &operator<<(QDBusArgument &arg, const FilterRule &rule) {
    arg.beginStructure(); arg << rule.type << rule.pattern; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, FilterRule &rule) {
    arg.beginStructure(); arg >> rule.type >> rule.pattern; arg.endStructure(); return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const Filter &filter) {
    arg.beginStructure(); arg << filter.label << filter.rules; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, Filter &filter) {
    arg.beginStructure(); arg >> filter.label >> filter.rules; arg.endStructure(); return arg;
}
}

QString FileDialog::choose(bool save, const QString &location, const QString &label,
                           const QStringList &patterns, QString *error) {
    qDBusRegisterMetaType<FilterRule>();
    qDBusRegisterMetaType<FilterRules>();
    qDBusRegisterMetaType<Filter>();
    qDBusRegisterMetaType<Filters>();
    FileDialog dialog;
    const auto bus = QDBusConnection::sessionBus();
    const QString token = "hype_" + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    Filter filter{label, {}};
    for (const auto &pattern : patterns) filter.rules.append({0, pattern});
    QByteArray folder = (save ? QFileInfo(location).absolutePath() : QDir(location).absolutePath()).toUtf8();
    folder.append('\0');
    QVariantMap options{{"handle_token", token}, {"modal", true}, {"multiple", false},
                        {"accept_label", save ? "Save" : "Open"}, {"current_folder", folder},
                        {"filters", QVariant::fromValue(Filters{filter})},
                        {"current_filter", QVariant::fromValue(filter)}};
    if (save) options.insert("current_name", QFileInfo(location).fileName());
    // Subscribe before sending: a fast response may arrive before the method reply.
    if (!bus.isConnected() || !dialog.listen("/org/freedesktop/portal/desktop/request/" + sender + "/" + token)) {
        *error = "Could not connect to the desktop file chooser.";
        return {};
    }
    const auto cleanup = qScopeGuard([&] {
        QCoreApplication::instance()->removeEventFilter(&dialog);
        if (!dialog.m_done)
            bus.asyncCall(QDBusMessage::createMethodCall(service, dialog.m_request, requestInterface, "Close"));
        dialog.disconnectRequest();
    });
    // A portal that exits before answering never sends its response, and the
    // event filter below would otherwise leave the windows locked for good.
    QDBusServiceWatcher portal(service, bus, QDBusServiceWatcher::WatchForUnregistration);
    connect(&portal, &QDBusServiceWatcher::serviceUnregistered, &dialog, [&] {
        if (dialog.m_done) return;
        dialog.m_error = "The desktop file chooser closed without answering.";
        dialog.m_done = true;
        dialog.m_loop.quit();
    });
    auto call = QDBusMessage::createMethodCall(service, "/org/freedesktop/portal/desktop",
                                              "org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile");
    call.setArguments({QString(), save ? QString("Save File") : QString("Open File"), options});
    QDBusPendingCallWatcher watcher(bus.asyncCall(call));
    connect(&watcher, &QDBusPendingCallWatcher::finished, &dialog, [&] {
        if (dialog.m_done) return;
        QDBusPendingReply<QDBusObjectPath> reply = watcher;
        if (reply.isError()) {
            dialog.m_error = "Could not open the desktop file chooser: " + reply.error().message();
            dialog.m_done = true;
            dialog.m_loop.quit();
        } else if (reply.value().path() != dialog.m_request) {
            dialog.disconnectRequest();
            if (!dialog.listen(reply.value().path())) {
                dialog.m_error = "Could not receive the desktop file chooser response.";
                dialog.m_loop.quit();
            }
        }
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &dialog.m_loop, &QEventLoop::quit);
    // The chooser lives in another process. Preserve modal behavior in our own
    // windows, so the selected slide/document cannot change during an import.
    QCoreApplication::instance()->installEventFilter(&dialog);
    if (!dialog.m_done) dialog.m_loop.exec();
    *error = dialog.m_error;
    return dialog.m_file;
}

bool FileDialog::listen(const QString &path) {
    m_request = path;
    return QDBusConnection::sessionBus().connect(service, path, requestInterface, "Response",
                                                this, SLOT(response(uint,QVariantMap)));
}
void FileDialog::disconnectRequest() {
    QDBusConnection::sessionBus().disconnect(service, m_request, requestInterface, "Response",
                                            this, SLOT(response(uint,QVariantMap)));
}
void FileDialog::response(uint result, const QVariantMap &values) {
    if (m_done) return;
    m_done = true;
    if (result == 0) {
        const auto uris = values.value("uris").toStringList();
        const QUrl url(uris.value(0));
        if (url.isLocalFile()) m_file = url.toLocalFile();
        else m_error = "Choose a local file.";
    } else if (result != 1) {
        m_error = "The desktop file chooser failed.";
    }
    m_loop.quit();
}
bool FileDialog::eventFilter(QObject *, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::Shortcut: case QEvent::ShortcutOverride:
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease: case QEvent::MouseButtonDblClick:
    case QEvent::Wheel: case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd:
    case QEvent::DragEnter: case QEvent::DragMove: case QEvent::Drop: case QEvent::Close:
        event->ignore();
        return true;
    default:
        return false;
    }
}
