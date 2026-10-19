#pragma once
#include <QAbstractListModel>
#include <QColor>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QImage>
#include <QMutex>
#include <QSet>
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
    QString definitions; // The <Define> blocks on its slides.
    QString error;
    int errorOffset = -1; // Where the unfinished front matter or code fence opens.
};
ParsedDeck parseDeck(const QString &source);
QString scalar(const QString &header, const QString &key, const QString &fallback = {});
QString setScalar(QString header, const QString &key, const QString &value);

class Deck : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY changed)
    Q_PROPERTY(QString slideSource READ slideSource NOTIFY selectionChanged)
    Q_PROPERTY(QString slideText READ slideText NOTIFY selectionChanged)
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY selectionChanged)
    Q_PROPERTY(int selectionFirst READ selectionFirst NOTIFY selectionChanged)
    Q_PROPERTY(int selectionLast READ selectionLast NOTIFY selectionChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    Q_PROPERTY(QString speakerNotes READ speakerNotes NOTIFY selectionChanged)
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
    Q_PROPERTY(QStringList fontNames READ fontNames NOTIFY fontsChanged)
    Q_PROPERTY(QString fontName READ fontName NOTIFY fontChanged)
    Q_PROPERTY(QStringList themeNames READ themeNames CONSTANT)
    Q_PROPERTY(QStringList mediaPositions READ mediaPositions CONSTANT)
    Q_PROPERTY(QString themeName READ themeName NOTIFY changed)
    Q_PROPERTY(QString templateName READ templateName NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QVariantMap media READ media NOTIFY selectionChanged)
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
    QString speakerNotes() const;
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
    // Google Fonts families matching a search; empty until loadGoogleFonts has the catalog.
    Q_INVOKABLE QStringList googleFonts(const QString &query) const;
    Q_INVOKABLE void loadGoogleFonts();
    // Families the presentation or its template names, and those not yet on this machine.
    QStringList requestedFonts() const;
    QStringList missingFonts() const;
    // Downloads missing families from Google Fonts; returns what could not be fetched.
    QStringList fetchFonts();
    // The editor downloads missing fonts in the background as they are named.
    void enableFontDownloads();
    QStringList themeNames() const;
    QString themeName() const;
    QString templateName() const;
    QColor background() const;
    QColor foreground() const;
    QColor accent() const;
    QVariantMap palette() const;
    // The palette plus page numbers, for slides whose template shows them.
    QVariantMap slidePalette(int index) const;
    QVariantMap media() const;
    QStringList mediaPositions() const;
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
    // For PowerPoint, animations become videos and opaque slides are stored as JPEG.
    bool renderImages(const QString &directory, int width = 1920, bool powerPoint = false);
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void extendSelection(int index);
    Q_INVOKABLE void moveSelection(int direction);
    Q_INVOKABLE void dropSelection(int slot);
    Q_INVOKABLE void selectAt(int position);
    Q_INVOKABLE int sourcePosition() const;
    Q_INVOKABLE int sourceEndPosition() const;
    Q_INVOKABLE void editSource(const QString &value);
    Q_INVOKABLE void editSlide(const QString &value);
    Q_INVOKABLE void moveSlide(int from, int to);
    Q_INVOKABLE void addSlide();
    Q_INVOKABLE void duplicateSlide();
    Q_INVOKABLE void deleteSlide();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void chooseTheme(const QString &name);
    // Installed themes with their colors, the desktop's own first.
    Q_INVOKABLE QVariantList themeChoices() const;
    // Templates the presentation can build on, each with a preview render id.
    Q_INVOKABLE QVariantList templates() const;
    // Builds the presentation on a template, or on none for an empty name.
    Q_INVOKABLE void chooseTemplate(const QString &name);
    // The template's slides, drawn in this presentation, that a new slide can start from.
    Q_INVOKABLE QVariantList templateSlides() const;
    Q_INVOKABLE void addTemplateSlide(int index);
    // Adds the template's slides at these indices, in order, after the selection.
    Q_INVOKABLE void addTemplateSlides(const QVariantList &indices);
    // Adds the saved presentation to the template library; returns an error.
    Q_INVOKABLE QString saveAsTemplate(const QString &name, bool copy, bool replace);
    Q_INVOKABLE QString suggestedTemplateName() const;
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
    Q_INVOKABLE void setMediaPosition(const QString &position);
    Q_INVOKABLE void setMediaSize(double percent);
    Q_INVOKABLE void setStatus(const QString &status);
  signals:
    void changed(); // Document content, path, saved state, or media files; always followed by selectionChanged.
    void selectionChanged(); // Selection range or the selected slide's content/media.
    void fontChanged();
    void statusChanged();
    void opened(bool existing);
    void compressingImageChanged();
    void exportChanged();
    void exportAdvanced(double progress, const QString &message);
    void exportFinished(bool success);
    void fontsChanged();
    void googleFontsReady();
    void pasteRequested(const QString &name, const QString &extension, bool video);

  private:
    struct State {
        QString source;
        int selected;
        int anchor;
        ParsedDeck parsed;
    };
    mutable QString m_paletteHeader, m_paletteDefinitions, m_mediaSource, m_mediaBase, m_mediaStamp;
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
    mutable QString m_paletteStamp, m_templatePath, m_templateText, m_templateHeader;
    mutable int m_paletteEpoch = -1;
    mutable QString m_requestedKey;
    mutable QStringList m_requested;
    bool m_fontDownloads = false, m_fetchingFonts = false, m_loadingCatalog = false;
    QSet<QString> m_fontAttempts;
    void fetchFontsLater();
    mutable QMutex m_stampMutex;
    mutable QString m_stampKey, m_stampValue;
    mutable QStringList m_templateWatched; // Files the templates it builds on came from, or may.
    mutable bool m_followsDesktop = false; // Its theme is the desktop's, which may change.
    mutable QHash<QString, QPair<QString, QString>>
        m_templatePreviews; // Path: file stamp, render id.
    QString templatePreview(const QString &path, const QString &title) const;
    mutable QElapsedTimer m_stampClock;
    QString templateStamp(bool force = false) const;
    QString m_source, m_saved, m_path, m_status;
    ParsedDeck m_parsed;
    int m_selected = 0, m_anchor = 0, m_revision = 0;
    QVector<State> m_undo, m_redo;
    QMap<QString, QString> m_themes;
    QFileSystemWatcher m_watcher, m_mediaWatcher;
    QTimer m_reloadTimer, m_mediaTimer;
    QString m_notifiedFont;
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
    void watchMedia();
    void notifyChanged();
    void reloadExternal(const QString &disk);
};
