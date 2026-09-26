#include "apptheme.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

static QColor mix(const QColor &base, const QColor &ink, double amount) {
    return QColor::fromRgbF(base.redF() + (ink.redF() - base.redF()) * amount,
                            base.greenF() + (ink.greenF() - base.greenF()) * amount,
                            base.blueF() + (ink.blueF() - base.blueF()) * amount);
}
static QColor contrastInk(const QColor &color) {
    auto linear = [](double value) {
        return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
    };
    const double luminance = .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) +
                             .0722 * linear(color.blueF());
    return luminance > .179 ? QColor("#111111") : QColor("#ffffff");
}
AppTheme::AppTheme(QObject *parent)
    : AppTheme(qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state") +
                   "/omarchy/current",
               parent) {}
AppTheme::AppTheme(const QString &currentDirectory, QObject *parent)
    : QObject(parent), m_currentDirectory(currentDirectory) {
    m_reload.setSingleShot(true);
    m_reload.setInterval(100);
    connect(&m_reload, &QTimer::timeout, this, &AppTheme::reload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reload.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reload.start(); });
    reload();
}
void AppTheme::setPresentationPalette(const QVariantMap &palette) {
    if (m_presentationPalette == palette)
        return;
    m_presentationPalette = palette;
    reload();
}
void AppTheme::reload() {
    const QString theme = m_currentDirectory + "/theme", path = theme + "/colors.toml";
    QMap<QString, QColor> values;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression entry(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re");
        while (!file.atEnd()) {
            const auto match = entry.match(QString::fromUtf8(file.readLine()));
            if (match.hasMatch())
                values[match.captured(1)] = QColor(match.captured(2));
        }
    } else {
        // On other desktops, color the whole editor with the selected presentation theme.
        for (auto it = m_presentationPalette.cbegin(); it != m_presentationPalette.cend(); ++it) {
            const QColor color(it.value().toString());
            if (color.isValid())
                values[it.key()] = color;
        }
    }
    const QColor bg = values.value("background", QColor("#1a1b26"));
    const QColor fg = values.value("foreground", QColor("#c0caf5"));
    const QColor accent = values.value("accent", QColor("#7aa2f7"));
    const QColor selection = values.value("selection", mix(bg, accent, .3));
    const QVariantMap colors{{"background", bg},
                             {"foreground", fg},
                             {"accent", accent},
                             {"panel", mix(bg, fg, .025)},
                             {"button", values.value("lighter_background", mix(bg, fg, .08))},
                             {"hover", mix(bg, accent, .22)},
                             {"border", mix(bg, fg, .22)},
                             {"muted", mix(bg, fg, .62)},
                             {"selection", selection},
                             {"selectionText", contrastInk(selection)},
                             {"accentText", contrastInk(accent)},
                             {"accentHover", mix(accent, fg, .15)},
                             {"windowBorder", values.value("active_border_color", accent)},
                             {"error", values.value("red", QColor("#d94b4b"))}};
    // Popups take the desktop's window corners: square unless the theme rounds them.
    int rounding = 0;
    QStringList windowFiles;
    for (const auto &name : {"/hyprland.lua", "/hyprland.conf"}) {
        QFile window(theme + name);
        if (!window.open(QIODevice::ReadOnly))
            continue;
        windowFiles.append(theme + name);
        const QRegularExpression entry(R"re(^\s*rounding\s*=\s*(\d+))re");
        while (!window.atEnd()) {
            const auto match = entry.match(QString::fromUtf8(window.readLine()));
            if (match.hasMatch())
                rounding = match.captured(1).toInt();
        }
    }
    if (colors != m_colors || rounding != m_rounding) {
        m_colors = colors;
        m_rounding = rounding;
        emit changed();
    }
    // Theme switching replaces symlinks; atomic saves replace file inodes.
    // Watch their parents as well, and re-arm after every change.
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QStringList paths{m_currentDirectory, theme, path};
    paths.append(windowFiles);
    QString ancestor = QFileInfo(m_currentDirectory).absolutePath();
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    paths.append(ancestor);
    paths.removeDuplicates();
    for (const auto &entry : paths)
        if (QFileInfo::exists(entry))
            m_watcher.addPath(entry);
}
