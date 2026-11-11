#pragma once
#include <QFileSystemWatcher>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

// The hex colors in an Omarchy colors.toml by key, as written, quoted or bare,
// with or without a trailing note.
QMap<QString, QString> readThemeColors(const QString &path);

class AppTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(int rounding READ rounding NOTIFY changed)
  public:
    explicit AppTheme(QObject *parent = nullptr);
    explicit AppTheme(const QString &currentDirectory, QObject *parent = nullptr);
    QVariantMap colors() const { return m_colors; }
    int rounding() const { return m_rounding; }
  signals:
    void changed();

  private:
    void reload();
    QString m_currentDirectory;
    QVariantMap m_colors;
    int m_rounding = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_reload;
};
