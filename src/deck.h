#pragma once
#include <QAbstractListModel>
#include <QColor>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QImage>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
class QProcess;

struct Slide {
    QString source;
    int start = 0;
    int end = 0;
};
struct ParsedDeck {
    QString header;
    QVector<Slide> slides;
    QString error;
    int errorOffset = -1; // Where the unfinished front matter or code fence opens.
};
ParsedDeck parseDeck(const QString &source);
QString scalar(const QString &header, const QString &key, const QString &fallback = {});
QString setScalar(QString header, const QString &key, const QString &value);

class Deck : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY changed)
    Q_PROPERTY(QString slideSource READ slideSource NOTIFY changed)
    Q_PROPERTY(QString slideText READ slideText NOTIFY changed)
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY changed)
    Q_PROPERTY(int selectionFirst READ selectionFirst NOTIFY changed)
    Q_PROPERTY(int selectionLast READ selectionLast NOTIFY changed)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString sizeLabel READ sizeLabel NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool compressingImage READ compressingImage NOTIFY compressingImageChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY exportChanged)
    Q_PROPERTY(QString exportStatus READ exportStatus NOTIFY exportChanged)
    Q_PROPERTY(bool exportFailed READ exportFailed NOTIFY exportChanged)
    Q_PROPERTY(QStringList fontNames READ fontNames CONSTANT)
    Q_PROPERTY(QString fontName READ fontName NOTIFY changed)
    Q_PROPERTY(QStringList themeNames READ themeNames CONSTANT)
    Q_PROPERTY(QString themeName READ themeName NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QVariantMap palette READ palette NOTIFY changed)
    Q_PROPERTY(QVariantMap media READ media NOTIFY changed)
  public:
    explicit Deck(QObject *parent = nullptr, const QString &exportProgram = {});
    ~Deck() override;
    bool compressingImage() const { return m_compressingImage; }
    bool exporting() const { return m_exporting; }
    double exportProgress() const { return m_exportProgress; }
    QString exportStatus() const { return m_exportStatus; }
    bool exportFailed() const { return m_exportFailed; }
    bool loadExportSnapshot(const QString &path);
    Q_INVOKABLE void startExport(const QString &format, const QString &path);
    Q_INVOKABLE void cancelExport();
    enum { NumberRole = Qt::UserRole + 1 };
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString source() const { return m_source; }
    QString slideSource() const;
    QString slideText() const;
    int selected() const { return m_selected; }
    int selectionFirst() const { return qMin(m_anchor, m_selected); }
    int selectionLast() const { return qMax(m_anchor, m_selected); }
    int selectionCount() const { return selectionLast() - selectionFirst() + 1; }
    int count() const { return m_parsed.slides.size(); }
    int revision() const { return m_revision; }
    bool dirty() const { return m_source != m_saved; }
    QString path() const { return m_path; }
    QString title() const;
    qint64 totalBytes() const;
    QString sizeLabel() const;
    QString status() const { return m_status; }
    QStringList fontNames() const;
    QString fontName() const;
    Q_INVOKABLE void chooseFont(const QString &family);
    QStringList themeNames() const;
    QString themeName() const;
    QColor background() const;
    QColor foreground() const;
    QColor accent() const;
    QVariantMap palette() const;
    QVariantMap media() const;
    QString baseDir() const;
    QString dialogDirectory() const;
    QString slide(int index) const;
    bool loadPath(const QString &path, bool remember = true);
    bool reopenLastPresentation();
    bool savePath(const QString &path);
    bool saveCopyPath(const QString &path);
    void enableAutosave(const QString &recoveryDirectory = {});
    Q_INVOKABLE bool flushAutosave();
    Q_INVOKABLE QVariantList recoveryVersions() const;
    Q_INVOKABLE bool restoreVersion(const QString &name);
    bool exportPdf(const QString &path);
    bool exportPptx(const QString &path);
    bool renderImages(const QString &directory, int width = 1920, bool convertAnimations = false);
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void extendSelection(int index);
    Q_INVOKABLE void moveSelection(int direction);
    Q_INVOKABLE void dropSelection(int slot);
    Q_INVOKABLE void selectAt(int position);
    Q_INVOKABLE int sourcePosition() const;
    Q_INVOKABLE void editSource(const QString &value);
    Q_INVOKABLE void editSlide(const QString &value);
    Q_INVOKABLE void moveSlide(int from, int to);
    Q_INVOKABLE void addSlide();
    Q_INVOKABLE void duplicateSlide();
    Q_INVOKABLE void deleteSlide();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void chooseTheme(const QString &name);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs();
    Q_INVOKABLE void newDeck();
    Q_INVOKABLE void importDialog();
    Q_INVOKABLE bool importMedia(const QUrl &url, bool newSlide = false);
    Q_INVOKABLE bool pasteMedia();
    Q_INVOKABLE QString savePastedMedia(const QString &name);
    Q_INVOKABLE void cancelPaste();
    Q_INVOKABLE void exportDialog(const QString &format);
    Q_INVOKABLE QString renderId(int index) const;
    Q_INVOKABLE void matchImageBackground(bool enabled);
    Q_INVOKABLE void setMediaBackground(const QString &mode);
    Q_INVOKABLE void setMediaMode(const QString &mode);
    Q_INVOKABLE void setStatus(const QString &status);
  signals:
    void changed();
    void statusChanged();
    void opened(bool existing);
    void compressingImageChanged();
    void exportChanged();
    void exportAdvanced(double progress, const QString &message);
    void exportFinished(bool success);
    void pasteRequested(const QString &name, const QString &extension, bool video);

  private:
    struct State {
        QString source;
        int selected;
        int anchor;
        ParsedDeck parsed;
    };
    mutable QString m_paletteHeader, m_mediaSource, m_mediaBase;
    mutable QString m_sizeSource, m_sizeBase;
    mutable qint64 m_totalBytes = -1, m_assetBytes = 0;
    mutable QStringList m_sizeFiles;
    mutable QElapsedTimer m_sizeClock;
    struct RenderIdentity {
        QString source, base, id;
        QVariantMap palette;
        QElapsedTimer clock;
    };
    mutable QHash<int, RenderIdentity> m_renderIds;
    mutable QVariantMap m_mediaCache;
    mutable QVariantMap m_paletteCache;
    QString m_source, m_saved, m_path, m_status;
    ParsedDeck m_parsed;
    int m_selected = 0, m_anchor = 0, m_revision = 0;
    QVector<State> m_undo, m_redo;
    QMap<QString, QString> m_themes;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    bool m_externalChange = false;
    QString m_recoveryDirectory, m_checkpointSource, m_checkpointPath;
    QTimer m_autosaveTimer, m_autosaveDeadline;
    bool m_recovering = false;
    bool m_compressingImage = false;
    quint64 m_pasteGeneration = 0;
    bool m_exporting = false, m_exportFailed = false;
    double m_exportProgress = 0;
    QString m_exportStatus;
    QString m_exportProgram;
    QProcess *m_exportProcess = nullptr;
    bool m_exportCancelled = false;
    struct PendingPaste {
        QString source, extension, path, document;
        QByteArray data;
        bool video = false;
        int selected = 0;
    } m_paste;
    void apply(const QString &source, int selected, bool history = true, int anchor = -1,
               const ParsedDeck *structure = nullptr);
    void replaceHeader(const QString &header);
    void replaceSlides(const QStringList &slides, int selected, int anchor = -1);
    bool confirmDiscard();
    bool validateStructure(const QString &operation);
    bool checkpoint();
    void recoverDraft();
    void retireDraft();
    QString recoveryFolder() const;
    bool restoreSnapshot(const QByteArray &bytes, bool opening);
    void discoverThemes();
    void watch();
    void reloadExternal(const QString &disk);
};
