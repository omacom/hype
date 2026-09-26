#pragma once
#include "deck.h"
#include <QMutex>
#include <QQuickImageProvider>
#include <QQuickPaintedItem>
#include <QThreadPool>
#include <atomic>
#include <memory>
class QTextDocument;
struct Media {
    QString file, path, poster, error, background;
    bool video = false, span = false, loop = false, muted = false, autoplay = true;
    double overlay = 0;
    QString text;
};
QRectF mediaRect(const Media &media);
QImage softenedImage(const QImage &image, const QSizeF &slideSize);
QString withMedia(const QString &source, const QString &reference);
QString withMediaDirectives(const QString &source, const QStringList &remove,
                            const QStringList &add);
QString speakerNotes(const QString &source);
Media parseMedia(const QString &source, const QString &base);
QString ensurePoster(const QString &video, const QString &base);
QStringList slideProblems(const QString &source, const QString &base);
void layoutSlideText(QTextDocument &document, const QString &markdown, const QVariantMap &palette,
                     qreal fontSize, qreal width, bool centered, bool code);
void paintSlide(QPainter *painter, const QRectF &target, const QString &source, const QString &base,
                const QVariantMap &palette, QString *warning = nullptr, bool overlayOnly = false,
                bool backgroundOnly = false);
class SlideItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(Deck *deck READ deck WRITE setDeck NOTIFY deckChanged)
    Q_PROPERTY(bool overlayOnly MEMBER m_overlayOnly NOTIFY deckChanged)
  public:
    explicit SlideItem(QQuickItem *parent = nullptr);
    Deck *deck() const { return m_deck; }
    void setDeck(Deck *deck);
    void paint(QPainter *painter) override;
  signals:
    void deckChanged();

  private:
    Deck *m_deck = nullptr;
    bool m_overlayOnly = false;
};
class Thumbnails : public QQuickAsyncImageProvider {
  public:
    explicit Thumbnails(Deck *deck);
    ~Thumbnails() override;
    void shutdown();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

  private:
    QMutex m_submissions;
    std::shared_ptr<std::atomic_bool> m_stopping = std::make_shared<std::atomic_bool>(false);
    QThreadPool m_thumbnails, m_previews, m_cached, m_videos;
    std::shared_ptr<std::atomic_uint64_t> m_generation = std::make_shared<std::atomic_uint64_t>(0);
};
