#include "renderer.h"
#include "images.h"
#include "syntax.h"
#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSvgRenderer>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QWaitCondition>

static QRegularExpression mediaRe(R"(!\[([^\]]*)\]\((?:<([^>]+)>|([^\s)]+))\))");
namespace {
class ImageCache {
    QMutex mutex;
    QCache<QString, QImage> images;
  public:
    explicit ImageCache(int kilobytes) : images(kilobytes) {}
    QImage get(const QString &key) {
        QMutexLocker lock(&mutex);
        auto image = images.object(key);
        return image ? *image : QImage();
    }
    void put(const QString &key, const QImage &image, int minimumEntries = 0) {
        QMutexLocker lock(&mutex);
        const int cost = qMax(1, int(image.sizeInBytes() / 1024));
        // High-DPI frames are several times larger; keep room for a useful number of them.
        if (minimumEntries && images.maxCost() < cost * minimumEntries)
            images.setMaxCost(cost * minimumEntries);
        images.insert(key, new QImage(image), cost);
    }
};
}
static QString outsideCode(QString source, bool maskInline = true) {
    int position = 0, fenceLength = 0;
    QChar fence;
    static const QRegularExpression marker("^ {0,3}(`{3,}|~{3,})(.*)$");
    while (position < source.size()) {
        int end = source.indexOf('\n', position);
        if (end < 0)
            end = source.size();
        QString line = source.mid(position, end - position);
        auto match = marker.match(line);
        bool protectedLine = fenceLength > 0;
        if (match.hasMatch()) {
            QString run = match.captured(1);
            protectedLine = true;
            if (!fenceLength) {
                fence = run[0];
                fenceLength = run.size();
            } else if (run[0] == fence && run.size() >= fenceLength &&
                       match.captured(2).trimmed().isEmpty())
                fenceLength = 0;
        }
        if (protectedLine || line.startsWith("    "))
            source.replace(position, end - position, QString(end - position, ' '));
        position = end + 1;
    }
    if (!maskInline)
        return source;
    static const QRegularExpression inlineCode("(`+)([^`]|`(?!`))*?\\1");
    auto matches = inlineCode.globalMatch(source);
    QVector<QPair<int, int>> ranges;
    while (matches.hasNext()) {
        auto m = matches.next();
        ranges.append({m.capturedStart(), m.capturedLength()});
    }
    for (auto range : ranges)
        source.replace(range.first, range.second, QString(range.second, ' '));
    return source;
}
static QString withoutComments(QString source) {
    auto matches = QRegularExpression("<!--[\\s\\S]*?-->").globalMatch(outsideCode(source));
    QVector<QPair<int, int>> ranges;
    while (matches.hasNext()) {
        auto m = matches.next();
        ranges.append({m.capturedStart(), m.capturedLength()});
    }
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it)
        source.remove(it->first, it->second);
    return source;
}
static QString assetPath(const QString &base, QString file, bool video) {
    if (QFileInfo(file).isAbsolute())
        return file;
    return QDir(base).filePath(file.startsWith("images/") || file.startsWith("videos/")
                                   ? file
                                   : (video ? "videos/" : "images/") + file);
}
QString withMedia(const QString &source, const QString &reference) {
    QString visible = outsideCode(source);
    auto comments = QRegularExpression("<!--[\\s\\S]*?-->").globalMatch(visible);
    while (comments.hasNext()) {
        const auto comment = comments.next();
        visible.replace(comment.capturedStart(), comment.capturedLength(),
                        QString(comment.capturedLength(), ' '));
    }
    auto match = mediaRe.match(visible);
    QString updated = source;
    if (match.hasMatch())
        updated.replace(match.capturedStart(), match.capturedLength(), reference);
    else
        updated += "\n" + reference + "\n";
    return updated;
}
QString withMediaDirectives(const QString &source, const QStringList &remove,
                            const QStringList &add) {
    const QString marker = "\x01HYPE_MEDIA\x01";
    const QString marked = withMedia(source, marker);
    const int start = marked.indexOf(marker);
    const int length = source.size() - marked.size() + marker.size();
    if (start < 0 || length <= 0)
        return source;
    QString reference = source.mid(start, length);
    const int end = reference.indexOf("](");
    if (end < 2)
        return source;
    QString flags = reference.mid(2, end - 2).trimmed();
    static const QRegularExpression tokens(R"re(([a-z]+)(?:=("(?:[^"\\]|\\.)*"|[^\s]+))?)re");
    const auto first = tokens.match(flags);
    const bool directives =
        first.hasMatch() && first.capturedStart() == 0 &&
        (QStringList{"fit", "span", "loop", "muted"}.contains(first.captured(1)) ||
         !first.captured(2).isEmpty());
    QStringList kept = add;
    if (directives) {
        auto matches = tokens.globalMatch(flags);
        while (matches.hasNext()) {
            const auto token = matches.next();
            if (!remove.contains(token.captured(1)))
                kept << token.captured();
        }
    } else if (!flags.isEmpty()) {
        kept << "alt=\"" + flags.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }
    reference.replace(2, end - 2, kept.join(' '));
    return withMedia(source, reference);
}
static Media readMedia(const QString &source, const QString &base) {
    Media result;
    result.text = withoutComments(source);
    auto m = mediaRe.match(outsideCode(result.text));
    if (!m.hasMatch())
        return result;
    result.file = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
    result.video = QStringList{"mp4", "m4v", "mov", "webm", "mkv"}.contains(
        QFileInfo(result.file).suffix().toLower());
    result.path = assetPath(base, result.file, result.video);
    result.text.remove(m.capturedStart(), m.capturedLength());
    result.span = !result.video &&
                  outsideCode(result.text)
                      .contains(QRegularExpression("^# ", QRegularExpression::MultilineOption));
    QString flags = m.captured(1).trimmed();
    static const QRegularExpression tokens(R"re(([a-z]+)(?:=("(?:[^"\\]|\\.)*"|[^\s]+))?)re");
    auto first = tokens.match(flags);
    bool directives =
        first.hasMatch() && first.capturedStart() == 0 &&
        (QStringList{"fit", "span", "loop", "muted"}.contains(first.captured(1)) ||
         !first.captured(2).isEmpty());
    QString explicitOverlay;
    bool fit = false, span = false;
    if (directives) {
        auto it = tokens.globalMatch(flags);
        int consumed = 0;
        while (it.hasNext()) {
            auto token = it.next();
            if (!flags.mid(consumed, token.capturedStart() - consumed).trimmed().isEmpty())
                result.error = "Invalid media directive";
            consumed = token.capturedEnd();
            QString key = token.captured(1), value = token.captured(2);
            if (value.startsWith('"'))
                value = value.mid(1, value.size() - 2).replace("\\\"", "\"").replace("\\\\", "\\");
            if (key == "span") {
                result.span = true;
                span = true;
            } else if (key == "fit") {
                result.span = false;
                fit = true;
            } else if (key == "loop")
                result.loop = value != "false";
            else if (key == "muted")
                result.muted = value != "false";
            else if (key == "autoplay")
                result.autoplay = value != "false";
            else if (key == "overlay")
                explicitOverlay = value;
            else if (key == "background") {
                result.background = value;
                if (value != "auto" && value != "theme" && value != "blur" && !QColor(value).isValid())
                    result.error = "Invalid background color";
            } else if (key == "poster")
                result.poster = assetPath(base, value, false);
            else if (key != "alt")
                result.error = "Unknown media directive: " + key;
        }
        if (!flags.mid(consumed).trimmed().isEmpty())
            result.error = "Invalid media directive";
    }
    if (fit && span)
        result.error = "Choose either span or fit";
    // A background choice implies fitting unless span was explicitly requested.
    if (!span && (result.background == "blur" || result.background == "auto"))
        result.span = false;
    result.overlay = (!result.video || result.span) && !result.text.trimmed().isEmpty() ? 0.25 : 0;
    if (!explicitOverlay.isEmpty()) {
        bool ok;
        double opacity = explicitOverlay.toDouble(&ok);
        if (!ok || opacity < 0 || opacity > 1)
            result.error = "Overlay must be between 0 and 1";
        else
            result.overlay = opacity;
    }
    return result;
}
Media parseMedia(const QString &source, const QString &base) {
    // Parsing is pure: file existence and modification checks remain at call sites.
    static thread_local QCache<QString, Media> cache(8 * 1024 * 1024);
    const QString key = base + QChar(0) + source;
    if (const auto result = cache.object(key))
        return *result;
    const auto result = readMedia(source, base);
    cache.insert(key, new Media(result), qMax(1, int((key.size() + result.text.size()) * 2)));
    return result;
}
static QString createPoster(const QString &video, const QString &base) {
    QFileInfo info(video);
    if (!info.exists())
        return {};
    QFile file(video);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    const QString digest = QString::fromLatin1(hash.result().toHex().left(16));
    QString path = base + "/images/.hype-poster-" + digest + ".jpg";
    if (QFile::exists(path))
        return path;
    QDir().mkpath(base + "/images");
    QTemporaryFile poster(base + "/images/.hype-poster-XXXXXX.jpg");
    if (!poster.open()) return {};
    poster.close();
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-y", "-i", video, "-frames:v", "1", "-vf",
                            "scale=1280:-2", poster.fileName()});
    if (!ffmpeg.waitForFinished(30000) || ffmpeg.exitCode() != 0) {
        ffmpeg.kill();
        ffmpeg.waitForFinished();
        return {};
    }
    // Parallel thumbnail/preview requests must never read a half-written poster.
    if (!QFile::exists(path) && !QFile::rename(poster.fileName(), path) && !QFile::exists(path))
        return {};
    return path;
}
QString ensurePoster(const QString &video, const QString &base) {
    static QMutex mutex;
    static QWaitCondition ready;
    static QSet<QString> pending;
    static QCache<QString, QString> cache(1024);
    const QFileInfo file(video);
    const QString key = base + QChar(0) + video + ':' + QString::number(file.size()) + ':' +
                        QString::number(file.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&mutex);
        while (pending.contains(key))
            ready.wait(&mutex);
        if (auto path = cache.object(key); path && QFileInfo::exists(*path))
            return *path;
        pending.insert(key);
    }
    const QString result = createPoster(video, base);
    {
        QMutexLocker lock(&mutex);
        if (!result.isEmpty())
            cache.insert(key, new QString(result));
        pending.remove(key);
        ready.wakeAll();
    }
    return result;
}
QStringList slideProblems(const QString &source, const QString &base) {
    QStringList errors;
    auto media = parseMedia(source, base);
    if (!media.error.isEmpty())
        errors << media.error;
    if (!media.file.isEmpty() && !QFileInfo::exists(media.path))
        errors << "Missing media: " + media.file;
    if (!media.file.isEmpty() && !media.video && QFileInfo::exists(media.path) &&
        !QImageReader(media.path).canRead())
        errors << "Cannot decode image: " + media.file;
    if (!media.poster.isEmpty() && !QFileInfo::exists(media.poster))
        errors << "Missing poster";
    auto matches = mediaRe.globalMatch(outsideCode(withoutComments(source)));
    int count = 0;
    while (matches.hasNext()) {
        matches.next();
        ++count;
    }
    if (count > 1)
        errors << "Use one media item per slide (combine artwork before importing)";
    return errors;
}
static QImage loadedImage(const QString &path, QSize canvas, bool span) {
    static ImageCache cache(256 * 1024);
    const QFileInfo info(path);
    QString key = path + QString::number(info.lastModified().toMSecsSinceEpoch()) + ":" +
                  QString::number(info.size()) + ":" + QString::number(canvas.width()) + "x" +
                  QString::number(canvas.height()) + (span ? ":span" : ":fit");
    if (const auto image = cache.get(key); !image.isNull())
        return image;
    QImage image = readSizedImage(path, canvas, span);
    if (span && !image.isNull()) {
        // Embed only the visible center crop, especially in PDF. Keep originals
        // intact so switching between Fit and Span remains reversible.
        QSize crop = image.size().scaled(canvas, Qt::KeepAspectRatioByExpanding);
        const double scale = double(image.width()) / crop.width();
        const QSize visible(qRound(canvas.width() * scale), qRound(canvas.height() * scale));
        if (visible != image.size())
            image = image.copy((image.width() - visible.width()) / 2,
                               (image.height() - visible.height()) / 2,
                               visible.width(), visible.height());
    }
    if (!image.isNull())
        cache.put(key, image);
    return image;
}
static QImage boxBlur(QImage image, int radius) {
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = image.width(), height = image.height(), diameter = radius * 2 + 1;
    // Three separable box passes approximate a Gaussian. Clamp the edges and
    // average premultiplied channels so transparent pictures keep clean edges.
    // Sums stay exact integers; a table replaces four divisions per pixel.
    QVector<uchar> average(255 * diameter + 1);
    for (int sum = 0; sum < average.size(); ++sum)
        average[sum] = uchar(sum / diameter);
    auto pixel = [&](int sum[4]) {
        return qRgba(average[sum[0]], average[sum[1]], average[sum[2]], average[sum[3]]);
    };
    auto add = [](int sum[4], QRgb color, int sign) {
        sum[0] += sign * qRed(color); sum[1] += sign * qGreen(color);
        sum[2] += sign * qBlue(color); sum[3] += sign * qAlpha(color);
    };
    QImage output(image.size(), image.format());
    std::vector<int> columns(size_t(width) * 4);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            const auto *in = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            auto *out = reinterpret_cast<QRgb *>(output.scanLine(y));
            int sum[4] = {};
            for (int i = -radius; i <= radius; ++i) add(sum, in[qBound(0, i, width - 1)], 1);
            for (int x = 0; x < width; ++x) {
                out[x] = pixel(sum);
                add(sum, in[qBound(0, x - radius, width - 1)], -1);
                add(sum, in[qBound(0, x + radius + 1, width - 1)], 1);
            }
        }
        // Vertical: keep a running sum per column and walk whole rows, so the pass
        // reads memory in order instead of striding down each column.
        std::fill(columns.begin(), columns.end(), 0);
        auto row = [&](int y) {
            return reinterpret_cast<const QRgb *>(output.constScanLine(qBound(0, y, height - 1)));
        };
        for (int i = -radius; i <= radius; ++i)
            for (int x = 0, *sum = columns.data(); x < width; ++x, sum += 4) add(sum, row(i)[x], 1);
        for (int y = 0; y < height; ++y) {
            auto *out = reinterpret_cast<QRgb *>(image.scanLine(y));
            const QRgb *leaving = row(y - radius), *entering = row(y + radius + 1);
            for (int x = 0, *sum = columns.data(); x < width; ++x, sum += 4) {
                out[x] = pixel(sum);
                add(sum, leaving[x], -1);
                add(sum, entering[x], 1);
            }
        }
    }
    return image;
}
static QImage blurredBackground(const QImage &image) {
    static ImageCache cache(16 * 1024);
    const QString key = QString::number(image.cacheKey());
    QImage blurred = cache.get(key);
    if (blurred.isNull()) {
        blurred = boxBlur(image.scaled(320, 180, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 8);
        cache.put(key, blurred);
    }
    return blurred;
}
QImage softenedImage(const QImage &image, const QSizeF &slideSize) {
    // A roughly two-pixel softness at 1080p, scaled with the picture at 4K.
    // Text is painted afterwards and stays sharp. Share the cache across preview
    // workers so changing a headline doesn't blur the same picture again.
    if (image.isNull() || slideSize.isEmpty()) return image;
    const int radius = qRound(2.0 * image.width() / slideSize.width());
    if (radius == 0) return image;
    static ImageCache cache(64 * 1024);
    const QString key = QString::number(image.cacheKey()) + '/' + QString::number(radius);
    QImage softened = cache.get(key);
    if (softened.isNull()) {
        softened = boxBlur(image, radius);
        cache.put(key, softened);
    }
    return softened;
}
static QString slideProperty(const QString &source, const QString &key) {
    QRegularExpression re("<!--\\s*hype:[\\s\\S]*?\\b" + key + "=\"([^\"]*)\"[\\s\\S]*?-->");
    return re.match(source).captured(1);
}
static QString preserveLineBreaks(QString markdown) {
    markdown.replace("\r\n", "\n").replace('\r', '\n');
    const QStringList visible = outsideCode(markdown, false).split('\n');
    QStringList lines = markdown.split('\n');
    for (int i = 0; i + 1 < lines.size(); ++i) {
        if (!visible[i].trimmed().isEmpty() && !lines[i].endsWith("  ") && !lines[i].endsWith('\\'))
            lines[i] += "  ";
    }
    return lines.join('\n');
}
static void sizeSlideText(QTextDocument &doc, const QVariantMap &palette, qreal fontSize,
                          qreal width, bool centered, bool code) {
    // A null page size suspends layout while every format below changes; the
    // final setTextWidth lays the document out once instead of once per run.
    doc.setPageSize(QSizeF(0, 0));
    QFont font(code ? QString("JetBrains Mono")
                    : palette.value("font", "JetBrains Mono").toString());
    font.setPixelSize(qRound(fontSize));
    font.setHintingPreference(QFont::PreferNoHinting);
    doc.setDefaultFont(font);
    doc.setDocumentMargin(0);
    QTextOption option;
    option.setUseDesignMetrics(true);
    option.setWrapMode(code ? QTextOption::NoWrap : QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(option);
    doc.setDefaultStyleSheet(
        QString("body { color: %1; } a { color: %2; } pre { white-space: pre; }")
            .arg(palette["foreground"].toString(), palette["accent"].toString()));
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat bf = block.blockFormat();
        int level = bf.headingLevel();
        bf.setAlignment(centered ? Qt::AlignHCenter : Qt::AlignLeft);
        bf.setTopMargin(level ? fontSize * 0.15 : 0);
        bf.setBottomMargin(fontSize * 0.22);
        bf.setLineHeight(115, QTextBlockFormat::ProportionalHeight);
        if (code) {
            bf.setBottomMargin(0);
            bf.setTopMargin(0);
            bf.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
        }
        cursor.setBlockFormat(bf);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCursor text(&doc);
            text.setPosition(fragment.position());
            text.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            QTextCharFormat cf;
            QFont f = font;
            f.setPixelSize(qRound(fontSize * (level == 1 ? 1.8 : level ? 1.25 : 1.0)));
            cf.setProperty(QTextFormat::FontPixelSize, f.pixelSize());
            cf.setFontFamilies({font.family()});
            // Qt drops both heading weight and its relative size at inline
            // formatting boundaries. The relative size overrides FontPixelSize
            // during layout, so restore both to preserve the existing heading
            // appearance, while keeping underline/italic/etc. local to each run.
            if (level) {
                cf.setFontWeight(QFont::Bold);
                cf.setProperty(QTextFormat::FontSizeAdjustment, 4 - level);
            }
            QColor color(palette["foreground"].toString());
            if (fragment.charFormat().fontWeight() >= QFont::Bold && !level)
                color = QColor(palette["accent"].toString());
            if (block.text().startsWith(QString::fromUtf8("—"))) {
                cf.setProperty(QTextFormat::FontPixelSize, qRound(fontSize * 0.7));
            }
            cf.setForeground(color);
            text.mergeCharFormat(cf);
        }
    }
    for (auto it = doc.rootFrame()->begin(); !it.atEnd(); ++it)
        if (auto table = qobject_cast<QTextTable *>(it.currentFrame())) {
            auto fmt = table->format();
            fmt.setBorder(0);
            fmt.setCellPadding(fontSize * 0.2);
            fmt.setCellSpacing(fontSize * 0.1);
            fmt.setWidth(QTextLength(QTextLength::PercentageLength, 100));
            table->setFormat(fmt);
        }
    doc.setTextWidth(width);
}
void layoutSlideText(QTextDocument &doc, const QString &markdown, const QVariantMap &palette,
                     qreal fontSize, qreal width, bool centered, bool code) {
    doc.setUndoRedoEnabled(false);
    doc.setMarkdown(preserveLineBreaks(markdown), QTextDocument::MarkdownDialectGitHub);
    sizeSlideText(doc, palette, fontSize, width, centered, code);
}
static QMutex fitMutex;
static QCache<QString, qreal> fittedSizes(4096);
static qreal fittedSize(const QString &key) {
    QMutexLocker lock(&fitMutex);
    const auto size = fittedSizes.object(key);
    return size ? *size : -1;
}
static void rememberFit(const QString &key, qreal size) {
    QMutexLocker lock(&fitMutex);
    fittedSizes.insert(key, new qreal(size));
}
QRectF mediaRect(const Media &media) {
    return media.span ? QRectF(0, 0, 1920, 1080)
                      : (!media.video || media.text.trimmed().isEmpty() ? QRectF(70, 50, 1780, 980)
                                                        : QRectF(100, 280, 1720, 730));
}
void paintSlide(QPainter *p, const QRectF &target, const QString &source, const QString &base,
                const QVariantMap &inputPalette, QString *warning, bool overlayOnly,
                bool backgroundOnly) {
    p->save();
    p->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                      QPainter::SmoothPixmapTransform);
    p->translate(target.topLeft());
    p->scale(target.width() / 1920.0, target.height() / 1080.0);
    QVariantMap palette = inputPalette;
    QString bg = slideProperty(source, "background"), fg = slideProperty(source, "foreground");
    if (QColor(bg).isValid())
        palette["background"] = bg;
    if (QColor(fg).isValid())
        palette["foreground"] = fg;
    if (!overlayOnly)
        p->fillRect(QRectF(0, 0, 1920, 1080), QColor(palette["background"].toString()));
    auto media = parseMedia(source, base);
    QString text = media.text.trimmed();
    auto problems = slideProblems(source, base);
    QRectF area(130, 90, 1660, 900);
    if (!media.file.isEmpty()) {
        QString path =
            media.video ? (media.poster.isEmpty() ? ensurePoster(media.path, base) : media.poster)
                        : media.path;
        const QRectF rect = mediaRect(media);
        const QSize pixels = p->deviceTransform().mapRect(rect).size().toSize();
        // An SVG with nothing over it is drawn as vectors, sharp at any size and in PDF. Its
        // small raster only gives its shape and edge colors for the background.
        const bool vector = !media.video && text.isEmpty() &&
                            QStringList{"svg", "svgz"}.contains(QFileInfo(path).suffix().toLower());
        QImage image = loadedImage(path, vector ? QSize(480, 270) : pixels, media.span);
        // Video backgrounds use the first frame, even with a custom poster.
        const QImage backdrop = media.video && !media.span && !media.poster.isEmpty() &&
            (media.background == "blur" || media.background == "auto")
            ? loadedImage(ensurePoster(media.path, base), QSize(320, 180), false) : image;
        if (!overlayOnly && !media.span && media.background == "blur") {
            if (!backdrop.isNull())
                p->drawImage(QRectF(0, 0, 1920, 1080), blurredBackground(backdrop));
        }
        if ((!media.video || !media.background.isEmpty()) && !media.span && media.background != "theme" &&
            (bg.isEmpty() || !media.background.isEmpty())) {
            QColor color(media.background);
            if ((media.background.isEmpty() || media.background == "auto") && !backdrop.isNull()) {
                // Quantized edge votes ignore transparent pixels and tolerate compression noise.
                QMap<int, QVector<QColor>> votes;
                for (int i = 0; i < 64; ++i) {
                    int x = i * (backdrop.width() - 1) / 63, y = i * (backdrop.height() - 1) / 63;
                    for (QPoint point : {QPoint(x, 0), QPoint(x, backdrop.height() - 1), QPoint(0, y),
                                         QPoint(backdrop.width() - 1, y)}) {
                        QColor c = backdrop.pixelColor(point);
                        if (c.alpha() < 240)
                            continue;
                        votes[(c.red() / 16) * 256 + (c.green() / 16) * 16 + c.blue() / 16].append(
                            c);
                    }
                }
                QVector<QColor> best;
                for (auto it = votes.cbegin(); it != votes.cend(); ++it)
                    if (it.value().size() > best.size())
                        best = it.value();
                if (best.size() >= 128) {
                    int r = 0, g = 0, b = 0;
                    for (const QColor &c : best) {
                        r += c.red();
                        g += c.green();
                        b += c.blue();
                    }
                    color = QColor(r / best.size(), g / best.size(), b / best.size());
                }
            }
            if (color.isValid()) {
                if (!overlayOnly)
                    p->fillRect(QRectF(0, 0, 1920, 1080), color);
                if (fg.isEmpty()) {
                    QString ink = (color.redF() * 0.2126 + color.greenF() * 0.7152 +
                                   color.blueF() * 0.0722) > .55
                                      ? "#161616"
                                      : "#ffffff";
                    palette["foreground"] = ink;
                    palette["accent"] = ink;
                }
            }
        }

        if (!overlayOnly && !backgroundOnly && !image.isNull()) {
            QSizeF scaled = image.size();
            scaled.scale(rect.size(),
                         media.span ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
            QRectF dest(QPointF(rect.center().x() - scaled.width() / 2,
                                rect.center().y() - scaled.height() / 2),
                        scaled);
            p->save();
            p->setClipRect(rect);
            QSvgRenderer svg;
            if (vector && svg.load(path)) {
                svg.setAspectRatioMode(Qt::KeepAspectRatio);
                svg.render(p, dest);
            } else
                p->drawImage(dest, !media.video && !text.isEmpty() ? softenedImage(image, dest.size()) : image);
            p->restore();
        } else if (!overlayOnly && !backgroundOnly) {
            p->setPen(QColor(palette["accent"].toString()));
            QFont diagnostic("sans");
            diagnostic.setPixelSize(32);
            p->setFont(diagnostic);
            p->drawText(rect, Qt::AlignCenter, "Missing media\n" + media.file);
        }
        if (!media.video || media.span) {
            if (!backgroundOnly)
                p->fillRect(QRectF(0, 0, 1920, 1080), QColor(0, 0, 0, qRound(media.overlay * 255)));
            if (fg.isEmpty() && !text.isEmpty())
                palette["foreground"] = "#ffffff";
        } else if (!text.isEmpty())
            area = QRectF(130, 40, 1660, 205);
    }
    if (backgroundOnly) {
        p->restore();
        return;
    }
    if (!text.isEmpty()) {
        bool code = text.contains(
            QRegularExpression("^ {0,3}(`{3,}|~{3,})", QRegularExpression::MultilineOption));
        bool quote = text.startsWith('>');
        bool list = text.contains(
            QRegularExpression("^\\s*(?:[-*+] |[0-9]+[.)] )", QRegularExpression::MultilineOption));
        bool table = text.contains(QRegularExpression("\\|[ :|-]+\\|"));
        bool centered = !(code || quote || list || table);
        const QString alignment = slideProperty(source, "alignment");
        if (alignment == "left")
            centered = false;
        if (alignment == "center")
            centered = true;
        bool stack = (text.contains('\n') || text.contains('\r')) && !text.startsWith('#') &&
                     !quote && !list && !code;
        qreal low = 8, high = code ? 56 : quote ? 64 : list ? 72 : table ? 60 : stack ? 128 : 76;
        if (media.video && !media.span)
            high = 48;
        QTextDocument doc;
        // Layout happens in 1080p slide units, so every render size, the PDF and
        // a theme change all reuse one search. Colors never affect the fit.
        const QString fit = QString("%1 %2 %3 %4 %5 ").arg(high).arg(area.width()).arg(area.height())
                                .arg(centered).arg(code) + palette.value("font").toString() + '\n' + text;
        if (const qreal fitted = fittedSize(fit); fitted > 0) {
            low = fitted;
            layoutSlideText(doc, text, palette, low, area.width(), centered, code);
        } else {
            layoutSlideText(doc, text, palette, high, area.width(), centered, code);
            const bool fits = doc.size().height() <= area.height() && doc.idealWidth() <= area.width() + 1;
            if (fits)
                low = high;
            for (int iteration = 0; !fits && iteration < 9; ++iteration) {
                qreal size = (low + high) / 2;
                sizeSlideText(doc, palette, size, area.width(), centered, code);
                if (doc.size().height() <= area.height() && doc.idealWidth() <= area.width() + 1)
                    low = size;
                else
                    high = size;
            }
            if (!fits)
                sizeSlideText(doc, palette, low, area.width(), centered, code);
            rememberFit(fit, low);
        }
        highlightCode(doc, palette);
        if (low < 24 && warning)
            *warning = "Text fits below 24px on a 1080p slide";
        p->save();
        p->translate(area.x(), area.y() + qMax(0.0, (area.height() - doc.size().height()) / 2));
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, QColor(palette["foreground"].toString()));
        doc.documentLayout()->draw(p, context);
        p->restore();
    }
    if (!problems.isEmpty()) {
        p->setPen(Qt::white);
        p->fillRect(QRectF(0, 1000, 1920, 80), QColor("#9b3030"));
        QFont diagnostic("sans");
        diagnostic.setPixelSize(21);
        p->setFont(diagnostic);
        p->drawText(QRectF(30, 1005, 1860, 70), Qt::AlignVCenter, problems.join(" · "));
        if (warning)
            *warning = problems.join("; ");
    }
    p->restore();
}
SlideItem::SlideItem(QQuickItem *parent) : QQuickPaintedItem(parent) { setAntialiasing(true); }
void SlideItem::setDeck(Deck *deck) {
    if (m_deck)
        disconnect(m_deck, nullptr, this, nullptr);
    m_deck = deck;
    if (deck)
        connect(deck, &Deck::changed, this, [this] { update(); });
    emit deckChanged();
    update();
}
void SlideItem::paint(QPainter *p) {
    if (m_deck)
        paintSlide(p, boundingRect(), m_deck->slideSource(), m_deck->baseDir(), m_deck->palette(),
                   nullptr, m_overlayOnly);
}
// Qt multiplies an Image's sourceSize by the screen's scale, so a 340px thumbnail arrives as
// 680px on a 2x display and the stage as 3840px. Classify requests against the largest one
// seen instead of fixed pixel sizes: the stage is the big one, everything else is small.
static std::atomic_int largestWidth{1920};
static std::atomic_int stageWidth{1920}, stageHeight{1080};
static bool stageSized(const QSize &dimensions) {
    int largest = largestWidth.load();
    while (dimensions.width() > largest && !largestWidth.compare_exchange_weak(largest, dimensions.width())) {}
    return dimensions.width() * 2 > largestWidth.load();
}
static ImageCache &slideCache(const QSize &dimensions) {
    // Full previews must not evict the much smaller sidebar and overview thumbnails.
    static ImageCache thumbnails(96 * 1024), previews(128 * 1024);
    return stageSized(dimensions) ? previews : thumbnails;
}
static QString slideCacheKey(const QString &id, const QSize &dimensions) {
    return id + QString::number(dimensions.width()) + "x" + QString::number(dimensions.height());
}
static QImage renderedSlide(const QString &id, QSize *size, const QSize &requested) {
    const QSize dimensions = requested.isValid() ? requested : QSize(320, 180);
    auto &renders = slideCache(dimensions);
    const QString key = slideCacheKey(id, dimensions);
    if (const auto cached = renders.get(key); !cached.isNull()) {
        if (size) *size = cached.size();
        return cached;
    }
    QByteArray bytes =
        QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding);
    QDataStream stream(bytes);
    QString source, base;
    QVariantMap palette;
    stream >> source >> base >> palette;
    if (stream.status() != QDataStream::Ok)
        return {};
    QImage image(dimensions, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    paintSlide(&p, image.rect(), source, base, palette, nullptr, id.endsWith("/overlay"),
               id.endsWith("/background"));
    p.end();
    // Room for the slides around the selection at full size, or a whole deck of thumbnails.
    renders.put(key, image, stageSized(dimensions) ? 14 : 400);
    if (size)
        *size = image.size();
    return image;
}

namespace {
class SlideResponse : public QQuickImageResponse {
    QImage m_image;
    std::atomic_bool m_cancelled = false;
  public:
    void cancel() override { m_cancelled = true; }
    void complete(const QImage &image) {
        if (!m_cancelled) m_image = image;
        emit finished();
    }
    void render(const QString &id, const QSize &size) {
        if (!m_cancelled) m_image = renderedSlide(id, nullptr, size);
        emit finished();
    }
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
};
}

Thumbnails::Thumbnails(Deck *deck) {
    // Full-size renders are the slow ones; give the stage and its prefetches most of the cores.
    const int cores = QThread::idealThreadCount();
    m_thumbnails.setMaxThreadCount(qBound(2, cores / 4, 4));
    m_previews.setMaxThreadCount(qBound(2, cores / 2, 6));
    m_cached.setMaxThreadCount(1);
    m_videos.setMaxThreadCount(2);
    auto timer = new QTimer(this);
    timer->setInterval(60);
    timer->setSingleShot(true);
    connect(deck, &Deck::changed, this, [this, timer] {
        if (m_stopping->load())
            return;
        ++*m_generation;
        timer->start();
    });
    connect(timer, &QTimer::timeout, this, [this, deck = QPointer<Deck>(deck)] {
        QMutexLocker submissions(&m_submissions);
        if (!deck || m_stopping->load())
            return;
        const auto generation = m_generation;
        const auto current = generation->load();
        for (int offset : {1, -1, 2, -2, 3, -3}) {
            const int index = deck->selected() + offset;
            if (index < 0 || index >= deck->count())
                continue;
            // Do not start video decoding or animation playback speculatively.
            const auto media = parseMedia(deck->slide(index), deck->baseDir());
            QImageReader reader(media.path);
            if (media.video || (!media.path.isEmpty() && reader.supportsAnimation() && reader.imageCount() > 1))
                continue;
            const QString id = deck->renderId(index);
            // Prefetch at the size the stage actually asks for, or the work is never used.
            const QSize size(stageWidth.load(), stageHeight.load());
            m_previews.start([generation, current, id, size] {
                if (generation->load() == current)
                    renderedSlide(id, nullptr, size);
            }, -1);
        }
    });
    timer->start();
}
Thumbnails::~Thumbnails() { shutdown(); }
void Thumbnails::shutdown() {
    {
        // Fence submissions before draining. Qt Quick can retain this provider
        // after the engine dies; no later request may start touching Qt fonts.
        QMutexLocker submissions(&m_submissions);
        m_stopping->store(true);
        ++*m_generation;
    }
    m_thumbnails.waitForDone();
    m_previews.waitForDone();
    m_cached.waitForDone();
    m_videos.waitForDone();
}
QImage Thumbnails::requestImage(const QString &id, QSize *size, const QSize &requested) {
    // Synchronous entry point used by rendering tests, not the QML engine.
    QMutexLocker submissions(&m_submissions);
    return m_stopping->load() ? QImage() : renderedSlide(id, size, requested);
}
QQuickImageResponse *Thumbnails::requestImageResponse(const QString &id, const QSize &requested) {
    QMutexLocker submissions(&m_submissions);
    auto response = new SlideResponse;
    const auto stopping = m_stopping;
    if (stopping->load()) {
        // Finished responses are valid even before the loader connects its
        // signal handler; QQuickImageResponse records its finished state.
        response->complete({});
        return response;
    }
    const QSize dimensions = requested.isValid() ? requested : QSize(320, 180);
    const QImage cached = slideCache(dimensions).get(slideCacheKey(id, dimensions));
    if (!cached.isNull()) {
        // A ready image must not wait behind unrelated decoding or prefetches.
        m_cached.start([response, cached, stopping] {
            response->complete(stopping->load() ? QImage() : cached);
        });
        return response;
    }
    QByteArray bytes =
        QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding);
    QDataStream stream(bytes);
    QString source, base;
    stream >> source >> base;
    // Video poster decoding cannot occupy the workers needed for ordinary slides.
    const bool video = parseMedia(source, base).video;
    const bool stage = stageSized(dimensions);
    if (stage) {
        stageWidth.store(dimensions.width());
        stageHeight.store(dimensions.height());
    }
    auto &pool = video ? m_videos : stage ? m_previews : m_thumbnails;
    pool.start(
        [response, id, requested, stopping] {
            if (stopping->load())
                response->complete({});
            else
                response->render(id, requested);
        },
        1);
    return response;
}
