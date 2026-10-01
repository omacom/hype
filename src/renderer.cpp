#include "renderer.h"
#include "budget.h"
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
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QWaitCondition>
#include <algorithm>
#include <limits>

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
static QString slideProperty(const QString &source, const QString &key) {
    QRegularExpression re("<!--\\s*hype:[\\s\\S]*?\\b" + key + "=\"([^\"]*)\"[\\s\\S]*?-->");
    return re.match(outsideCode(source)).captured(1);
}
static WordCloud readWordCloud(const QString &markdown) {
    WordCloud result;
    static const QRegularExpression heading("^(#{2,6})[ \\t]+\\S");
    for (const auto &line : withoutComments(markdown).split('\n')) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        const auto match = heading.match(trimmed);
        QTextDocument doc;
        doc.setMarkdown(trimmed, QTextDocument::MarkdownDialectGitHub);
        if (match.hasMatch() && result.footer.isEmpty()) {
            CloudLabel label;
            label.text = doc.toPlainText().trimmed();
            label.level = match.captured(1).size();
            if (label.text.isEmpty()) {
                result.error = "Word cloud labels cannot be empty";
                return result;
            }
            result.labels.append(label);
        } else if (trimmed.startsWith("> ")) {
            if (!result.footer.isEmpty()) result.footer += ' ';
            result.footer += doc.toPlainText().trimmed();
        } else {
            result.error = "Word cloud needs ## to ###### labels, followed by an optional > footer";
            return result;
        }
    }
    if (result.labels.isEmpty()) result.error = "Word cloud needs at least one ## to ###### label";
    return result;
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
    if (slideProperty(source, "layout") == "gallery") {
        result.layout = "gallery";
        for (const auto &line : result.text.split('\n')) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;
            const auto image = mediaRe.match(trimmed);
            if (trimmed.startsWith("# ") && result.heading.isEmpty() && result.gallery.isEmpty()) {
                result.heading = trimmed;
            } else if (trimmed.startsWith("## ") && !result.heading.isEmpty() && result.footer.isEmpty() &&
                       (result.gallery.isEmpty() || !result.gallery.last().file.isEmpty())) {
                result.gallery.append({trimmed.mid(3).trimmed(), {}, {}});
            } else if (image.hasMatch() && image.capturedStart() == 0 && image.capturedLength() == trimmed.size() &&
                       !result.gallery.isEmpty() && result.gallery.last().file.isEmpty() && result.footer.isEmpty()) {
                auto &item = result.gallery.last();
                item.file = image.captured(2).isEmpty() ? image.captured(3) : image.captured(2);
                item.path = assetPath(base, item.file, false);
            } else if (trimmed.startsWith("> ") && !result.gallery.isEmpty() && !result.gallery.last().file.isEmpty()) {
                if (!result.footer.isEmpty()) result.footer += '\n';
                result.footer += trimmed.mid(2);
            } else {
                result.error = "Gallery needs a # title, pairs of ## labels and images, then an optional > footer";
                break;
            }
        }
        if (result.heading.isEmpty() || result.gallery.isEmpty() ||
            (!result.gallery.isEmpty() && result.gallery.last().file.isEmpty()))
            result.error = "Gallery needs a # title and an image for every ## label";
        if (result.gallery.size() > 9)
            result.error = "Gallery supports at most nine groups per slide";
        return result;
    }
    if (slideProperty(source, "layout") == "cloud") {
        result.layout = "cloud";
        const auto heading = QRegularExpression("^# [^\\n]+", QRegularExpression::MultilineOption)
                                 .match(outsideCode(result.text));
        if (heading.hasMatch()) {
            result.heading = result.text.mid(heading.capturedStart(), heading.capturedLength());
            result.text.remove(heading.capturedStart(), heading.capturedLength());
        }
        result.error = readWordCloud(result.text).error;
        if (!heading.hasMatch()) result.error = "Word cloud needs a # title";
        if (m.hasMatch()) result.error = "Word cloud is a text-only layout";
        return result;
    }
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
            else if (key == "layout") {
                if (value != "title" && value != "overlay" && value != "split" && value != "caption" && value != "caption-right")
                    result.error = "Layout must be title, overlay, split, caption, or caption-right";
                else
                    result.layout = value;
            } else if (key == "background") {
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
    if (result.layout == "title" || result.layout == "caption" || result.layout == "caption-right") {
        const auto heading = QRegularExpression("^# [^\\n]+", QRegularExpression::MultilineOption)
                                 .match(outsideCode(result.text));
        if (heading.hasMatch()) {
            result.heading = result.text.mid(heading.capturedStart(), heading.capturedLength());
            result.text.remove(heading.capturedStart(), heading.capturedLength());
        } else if (result.layout == "title")
            result.error = "Title layout needs a # heading";
        if (!span)
            result.span = false;
    }
    if ((result.layout == "split" || result.layout == "caption" || result.layout == "caption-right") && !span)
        result.span = false;
    result.overlay = result.layout != "split" && result.layout != "caption" && result.layout != "caption-right" &&
                             (!result.video || result.span || result.layout == "title") &&
                             !result.text.trimmed().isEmpty() ? 0.25 : 0;
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
    for (const auto &item : media.gallery) {
        if (item.file.isEmpty()) continue;
        if (!QFileInfo::exists(item.path))
            errors << "Missing media: " + item.file;
        else if (!QImageReader(item.path).canRead())
            errors << "Cannot decode gallery image: " + item.file;
    }
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
    if (count > 1 && media.layout != "gallery")
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
                          qreal width, bool centered, bool code, bool equalColumns = false) {
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
            if (equalColumns)
                fmt.setColumnWidthConstraints(QList<QTextLength>(
                    table->columns(), QTextLength(QTextLength::PercentageLength, 100.0 / table->columns())));
            table->setFormat(fmt);
        }
    doc.setTextWidth(width);
}
void layoutSlideText(QTextDocument &doc, const QString &markdown, const QVariantMap &palette,
                     qreal fontSize, qreal width, bool centered, bool code, bool equalColumns) {
    doc.setUndoRedoEnabled(false);
    doc.setMarkdown(preserveLineBreaks(markdown), QTextDocument::MarkdownDialectGitHub);
    sizeSlideText(doc, palette, fontSize, width, centered, code, equalColumns);
}
WordCloud layoutWordCloud(const QString &markdown, const QVariantMap &palette, const QRectF &area) {
    WordCloud result = readWordCloud(markdown);
    if (!result.error.isEmpty()) return result;
    // Place larger labels first. Stable ordering and geometric candidates make
    // the result repeatable at every render size, without a random seed.
    std::stable_sort(result.labels.begin(), result.labels.end(), [](const auto &a, const auto &b) {
        return a.level < b.level;
    });
    const int sizes[] = {116, 88, 64, 50, 38};
    for (qreal scale = 1; scale >= 0.20; scale *= 0.94) {
        QVector<QRectF> occupied;
        bool fits = true;
        for (auto &label : result.labels) {
            QFont font(palette.value("font", "JetBrains Mono").toString());
            font.setHintingPreference(QFont::PreferNoHinting);
            font.setPixelSize(qMax(8, qRound(sizes[label.level - 2] * scale)));
            font.setWeight(label.level == 2 ? QFont::Bold : label.level == 3 ? QFont::DemiBold
                                                       : label.level == 4 ? QFont::Medium : QFont::Normal);
            const QFontMetricsF metrics(font);
            const qreal w = qMax(metrics.horizontalAdvance(label.text), metrics.boundingRect(label.text).width()) + 10;
            const qreal h = metrics.height() + 8;
            const qreal gap = 20 * scale;
            QVector<qreal> xs{area.center().x() - w / 2, area.left(), area.right() - w};
            QVector<qreal> ys{area.center().y() - h / 2, area.top(), area.bottom() - h};
            for (const auto &other : occupied) {
                xs << other.left() << other.right() - w << other.left() - gap - w << other.right() + gap;
                ys << other.top() << other.bottom() - h << other.top() - gap - h << other.bottom() + gap;
            }
            qreal bestScore = std::numeric_limits<qreal>::max();
            QRectF best;
            for (qreal x : xs) for (qreal y : ys) {
                const QRectF candidate(x, y, w, h);
                if (!area.contains(candidate)) continue;
                const qreal dx = (candidate.center().x() - area.center().x()) / area.width();
                const qreal dy = (candidate.center().y() - area.center().y()) / area.height();
                const qreal score = dx * dx + dy * dy;
                if (score >= bestScore) continue;
                bool collision = false;
                for (const auto &other : occupied)
                    if (candidate.adjusted(-gap / 2, -gap / 2, gap / 2, gap / 2).intersects(other)) {
                        collision = true;
                        break;
                    }
                if (!collision) {
                    best = candidate;
                    bestScore = score;
                }
            }
            if (best.isEmpty()) {
                fits = false;
                break;
            }
            label.font = font;
            label.rect = best;
            occupied.append(best);
        }
        if (fits) {
            QRectF bounds;
            for (const auto &rect : occupied) bounds = bounds.united(rect);
            const QPointF offset = area.center() - bounds.center();
            for (auto &label : result.labels) label.rect.translate(offset);
            return result;
        }
    }
    result.error = "Word cloud is too full; shorten labels or split the slide";
    return result;
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
    if (media.layout == "caption")
        return media.heading.isEmpty() ? QRectF(130, 100, 1660, 530)
                                       : QRectF(130, 190, 1660, 440);
    if (media.layout == "caption-right")
        return media.heading.isEmpty() ? QRectF(70, 50, 1780, 800)
                                       : QRectF(70, 180, 1780, 680);
    if (media.layout == "split")
        return QRectF(1030, 100, 790, 880);
    if (media.layout == "title")
        return QRectF(70, 200, 1780, 830);
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
    const QVariantMap headingPalette = palette;
    if (!overlayOnly)
        p->fillRect(QRectF(0, 0, 1920, 1080), QColor(palette["background"].toString()));
    auto media = parseMedia(source, base);
    const bool split = media.layout == "split";
    const bool fullCaption = media.layout == "caption";
    const bool caption = fullCaption || media.layout == "caption-right";
    QString text = media.text.trimmed();
    auto problems = slideProblems(source, base);
    QRectF area(130, 90, 1660, 900);
    if (!media.file.isEmpty()) {
        QString path =
            media.video ? (media.poster.isEmpty() ? ensurePoster(media.path, base) : media.poster)
                        : media.path;
        const QRectF rect = mediaRect(media);
        const QSize pixels = p->deviceTransform().mapRect(rect).size().toSize();
        QImage image = loadedImage(path, pixels, media.span);
        // Video backgrounds use the first frame, even with a custom poster.
        const QImage backdrop = media.video && !media.span && !media.poster.isEmpty() &&
            (media.background == "blur" || media.background == "auto")
            ? loadedImage(ensurePoster(media.path, base), QSize(320, 180), false) : image;
        if (!overlayOnly && !media.span && media.background == "blur") {
            if (!backdrop.isNull())
                p->drawImage(QRectF(0, 0, 1920, 1080), blurredBackground(backdrop));
        }
        if ((!media.video || !media.background.isEmpty()) && !media.span && media.background != "theme" &&
            (!(split || caption) || !media.background.isEmpty()) &&
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
            p->drawImage(dest, !media.video && !text.isEmpty() && !split && !caption ? softenedImage(image, dest.size()) : image);
            p->restore();
        } else if (!overlayOnly && !backgroundOnly) {
            p->setPen(QColor(palette["accent"].toString()));
            QFont diagnostic("sans");
            diagnostic.setPixelSize(32);
            p->setFont(diagnostic);
            p->drawText(rect, Qt::AlignCenter, "Missing media\n" + media.file);
        }
        if (split || caption) {
            if (!backgroundOnly)
                p->fillRect(rect, QColor(0, 0, 0, qRound(media.overlay * 255)));
            area = fullCaption ? QRectF(130, 650, 1660, 260)
                               : caption ? QRectF(1030, 870, 790, 160) : QRectF(130, 100, 800, 880);
        } else if (!media.video || media.span || media.layout == "title") {
            if (!backgroundOnly)
                p->fillRect(QRectF(0, 0, 1920, 1080),
                            QColor(0, 0, 0, qRound(media.overlay * 255)));
            if (fg.isEmpty() && !text.isEmpty())
                palette["foreground"] = "#ffffff";
        } else if (!text.isEmpty())
            area = QRectF(130, 40, 1660, 205);
        if (media.layout == "title")
            area = mediaRect(media).adjusted(60, 40, -60, -40);
    }
    if (backgroundOnly) {
        p->restore();
        return;
    }
    if (!media.heading.isEmpty()) {
        QTextDocument title;
        qreal size = 44;
        layoutSlideText(title, media.heading, headingPalette, size, 1660, true, false);
        while (size > 8 && (title.size().height() > 145 || title.idealWidth() > 1661)) {
            size = qMax(8.0, size - 2);
            sizeSlideText(title, headingPalette, size, 1660, true, false);
        }
        if (size < 24 && warning)
            *warning = "Title fits below 24px on a 1080p slide";
        p->save();
        p->translate(130, 25 + qMax(0.0, (145 - title.size().height()) / 2));
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, QColor(headingPalette["foreground"].toString()));
        title.documentLayout()->draw(p, context);
        p->restore();
    }
    if (media.layout == "gallery") {
        const int count = media.gallery.size();
        const int columns = qMin(3, count), rows = columns ? (count + columns - 1) / columns : 0;
        const QRectF grid(130, 200, 1660, media.footer.isEmpty() ? 790 : 710);
        auto drawText = [&](const QString &markdown, const QRectF &rect, qreal maximum) {
            QTextDocument doc;
            qreal size = maximum;
            layoutSlideText(doc, markdown, palette, size, rect.width(), true, false);
            while (size > 8 && (doc.size().height() > rect.height() || doc.idealWidth() > rect.width() + 1)) {
                size -= 2;
                sizeSlideText(doc, palette, size, rect.width(), true, false);
            }
            if (size < 24 && warning) *warning = "Gallery label fits below 24px on a 1080p slide";
            p->save();
            p->translate(rect.x(), rect.y() + qMax(0.0, (rect.height() - doc.size().height()) / 2));
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette.setColor(QPalette::Text, QColor(palette["foreground"].toString()));
            doc.documentLayout()->draw(p, context);
            p->restore();
        };
        for (int i = 0; i < count && count <= 9; ++i) {
            const auto &item = media.gallery[i];
            const int row = i / columns, column = i % columns;
            const int rowCount = qMin(columns, count - row * columns);
            const qreal width = (grid.width() - 50 * (columns - 1)) / columns;
            const qreal height = (grid.height() - 30 * (rows - 1)) / rows;
            const qreal x = grid.x() + (grid.width() - (rowCount * width + (rowCount - 1) * 50)) / 2 + column * (width + 50);
            const qreal y = grid.y() + row * (height + 30);
            drawText(item.label, QRectF(x, y, width, 60), 44);
            const QRectF rect(x, y + 75, width, height - 75);
            if (!overlayOnly) {
                const QImage picture = loadedImage(item.path, p->deviceTransform().mapRect(rect).size().toSize(), false);
                if (!picture.isNull()) {
                    QSizeF size = picture.size();
                    size.scale(rect.size(), Qt::KeepAspectRatio);
                    p->drawImage(QRectF(rect.center() - QPointF(size.width() / 2, size.height() / 2), size), picture);
                }
            }
        }
        if (!media.footer.isEmpty()) drawText(media.footer, QRectF(130, 950, 1660, 85), 36);
        text.clear();
    }
    if (media.layout == "cloud") {
        const bool footer = !readWordCloud(text).footer.isEmpty();
        const auto cloud = layoutWordCloud(text, palette, QRectF(120, 210, 1680, footer ? 720 : 790));
        if (!cloud.error.isEmpty()) {
            if (!problems.contains(cloud.error)) problems << cloud.error;
        } else {
            const QStringList colors{"accent", "magenta", "bright_foreground", "green", "foreground"};
            for (const auto &label : cloud.labels) {
                p->setFont(label.font);
                p->setPen(QColor(palette.value(colors[label.level - 2], palette["foreground"]).toString()));
                p->drawText(label.rect, Qt::AlignCenter | Qt::TextSingleLine, label.text);
                if (label.font.pixelSize() < 24 && warning)
                    *warning = "Word cloud text fits below 24px on a 1080p slide";
            }
            if (!cloud.footer.isEmpty()) {
                QFont font(palette.value("font", "JetBrains Mono").toString());
                font.setPixelSize(28);
                p->setFont(font);
                p->setPen(QColor(palette["foreground"].toString()));
                p->drawText(QRectF(130, 962, 1660, 76), Qt::AlignCenter | Qt::TextWordWrap, cloud.footer);
            }
        }
        text.clear();
    }
    if (!text.isEmpty()) {
        bool code = text.contains(
            QRegularExpression("^ {0,3}(`{3,}|~{3,})", QRegularExpression::MultilineOption));
        bool quote = text.startsWith('>');
        bool list = text.contains(
            QRegularExpression("^\\s*(?:[-*+] |[0-9]+[.)] )", QRegularExpression::MultilineOption));
        bool table = text.contains(QRegularExpression("\\|[ :|-]+\\|"));
        bool centered = fullCaption || !(code || quote || list || table || split);
        const bool equalColumns = fullCaption && table;
        const QString alignment = slideProperty(source, "alignment");
        if (alignment == "left")
            centered = false;
        if (alignment == "center")
            centered = true;
        bool stack = (text.contains('\n') || text.contains('\r')) && !text.startsWith('#') &&
                     !quote && !list && !code;
        qreal low = 8, high = code ? 56 : quote ? 64 : list ? 72 : table ? 60 : stack ? 128 : 76;
        if (fullCaption)
            high = 44;
        else if (split || caption)
            high = 48;
        else if (media.video && !media.span)
            high = 48;
        QTextDocument doc;
        // Layout happens in 1080p slide units, so every render size, the PDF and
        // a theme change all reuse one search. Colors never affect the fit.
        const QString fit = QString("%1 %2 %3 %4 %5 %6 ").arg(high).arg(area.width()).arg(area.height())
                                .arg(centered).arg(code).arg(equalColumns) + palette.value("font").toString() + '\n' + text;
        if (const qreal fitted = fittedSize(fit); fitted > 0) {
            low = fitted;
            layoutSlideText(doc, text, palette, low, area.width(), centered, code, equalColumns);
        } else {
            layoutSlideText(doc, text, palette, high, area.width(), centered, code, equalColumns);
            const bool fits = doc.size().height() <= area.height() && doc.idealWidth() <= area.width() + 1;
            if (fits)
                low = high;
            for (int iteration = 0; !fits && iteration < 9; ++iteration) {
                qreal size = (low + high) / 2;
                sizeSlideText(doc, palette, size, area.width(), centered, code, equalColumns);
                if (doc.size().height() <= area.height() && doc.idealWidth() <= area.width() + 1)
                    low = size;
                else
                    high = size;
            }
            if (!fits)
                sizeSlideText(doc, palette, low, area.width(), centered, code, equalColumns);
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
    // Full-size frames reach 33 MB at 2x, so keep as many as a twentieth of free memory holds.
    int frames = 14;
    if (const qint64 free = budget::availableBytes(); free > 0)
        frames = int(qBound(qint64(4), free / 20 / qMax(qint64(1), qint64(image.sizeInBytes())), qint64(14)));
    renders.put(key, image, stageSized(dimensions) ? frames : 400);
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
    // Full-size renders are the slow ones; give the stage and its prefetches most of the cores,
    // as far as memory allows: each full-size render holds about 100 MB while it works.
    const int cores = QThread::idealThreadCount();
    m_thumbnails.setMaxThreadCount(qBound(2, cores / 4, 4));
    m_previews.setMaxThreadCount(qMin(qBound(2, cores / 2, 6), budget::workers(100LL << 20, 6, 0.2, 0, 2)));
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
