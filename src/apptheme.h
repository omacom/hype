#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class AppTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(QVariantMap presentationPalette READ presentationPalette WRITE setPresentationPalette)
    Q_PROPERTY(int rounding READ rounding NOTIFY changed)
  public:
    explicit AppTheme(QObject *parent = nullptr);
    explicit AppTheme(const QString &currentDirectory, QObject *parent = nullptr);
    QVariantMap colors() const { return m_colors; }
    QVariantMap presentationPalette() const { return m_presentationPalette; }
    void setPresentationPalette(const QVariantMap &palette);
    int rounding() const { return m_rounding; }
  signals:
    void changed();

  private:
    void reload();
    QString m_currentDirectory;
    QVariantMap m_colors;
    QVariantMap m_presentationPalette;
    int m_rounding = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_reload;
};
