#include "animationexport.h"
#include "pptx.h"
#include "renderer.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QProcess>
#include <QScopeGuard>
#include <memory>
#include <webp/demux.h>

bool exportAnimation(const QString &source, const QString &base, const QVariantMap &palette,
                     const QString &output, int width, int *repeats, QString *error,
                     const std::function<void(double)> &progress) {
    const auto media = parseMedia(source, base);
    QImageReader reader(media.path);
    int frames = reader.imageCount();
    *repeats = media.loop || reader.loopCount() < 0 ? -1 : reader.loopCount() + 1;
    QByteArray webpBytes;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> webp(nullptr,
                                                                            WebPAnimDecoderDelete);
    WebPAnimInfo info{};
    if (reader.format() == "webp") {
        // libwebp composites disposal-to-background frames correctly, including
        // fully transparent replacements which Qt's WebP reader can retain.
        QFile file(media.path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = file.errorString();
            return false;
        }
        webpBytes = file.readAll();
        const WebPData data{reinterpret_cast<const uint8_t *>(webpBytes.constData()),
                            size_t(webpBytes.size())};
        webp.reset(WebPAnimDecoderNew(&data, nullptr));
        if (!webp || !WebPAnimDecoderGetInfo(webp.get(), &info)) {
            *error = "Cannot decode WebP animation";
            return false;
        }
        frames = int(info.frame_count);
        *repeats = media.loop || info.loop_count == 0 ? -1 : int(info.loop_count);
    }
    int previousTimestamp = 0;
    const QSize size(width, width * 9 / 16);
    QImage background(size, QImage::Format_RGB32);
    QImage overlay(size, QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::transparent);
    {
        QPainter p(&background);
        paintSlide(&p, background.rect(), source, base, palette, nullptr, false, true);
        QPainter op(&overlay);
        paintSlide(&op, overlay.rect(), source, base, palette, nullptr, true);
    }
    const QRectF rect = mediaRect(media);
    // Feed a bounded raw-frame pipe: no directory of full-size PNGs and no
    // complete animation in memory. Quantize cumulative timestamps to the same
    // 60 Hz output clock used by PowerPoint exports, avoiding per-frame drift.
    QProcess encoder;
    encoder.start("ffmpeg", {"-v",
                             "error",
                             "-nostdin",
                             "-y",
                             "-f",
                             "rawvideo",
                             "-pixel_format",
                             "rgba",
                             "-video_size",
                             QString("%1x%2").arg(size.width()).arg(size.height()),
                             "-framerate",
                             "60",
                             "-i",
                             "pipe:0",
                             "-an",
                             "-c:v",
                             "libx264",
                             "-threads",
                             QString::number(encoderThreads()),
                             "-preset",
                             "fast",
                             // x264 buffers this many 4K frames to plan bitrate; the default of
                             // 30 costs over a gigabyte for little gain on a short loop.
                             "-rc-lookahead",
                             "10",
                             "-crf",
                             "18",
                             "-pix_fmt",
                             "yuv420p",
                             "-movflags",
                             "+faststart",
                             QFileInfo(output).absoluteFilePath()});
    if (!encoder.waitForStarted()) {
        *error = "Cannot start ffmpeg for animation conversion: " + encoder.errorString();
        return false;
    }
    const auto cleanup = qScopeGuard([&] {
        if (encoder.state() != QProcess::NotRunning) {
            encoder.kill();
            encoder.waitForFinished();
        }
    });
    QByteArray diagnostics;
    auto writeFrame = [&](const QImage &frame) {
        const char *bytes = reinterpret_cast<const char *>(frame.constBits());
        qint64 remaining = frame.sizeInBytes();
        while (remaining > 0) {
            const qint64 count = encoder.write(bytes, qMin(remaining, qint64(256 * 1024)));
            if (count < 0)
                return false;
            bytes += count;
            remaining -= count;
            while (encoder.bytesToWrite() > 0) {
                if (!encoder.waitForBytesWritten(30000))
                    return false;
                diagnostics = (diagnostics + encoder.readAllStandardError()).right(8192);
            }
        }
        return true;
    };
    qint64 duration = 0, writtenFrames = 0;
    for (int i = 0; i < frames; ++i) {
        if (progress)
            progress(double(i) / frames);
        QImage frame;
        int delay;
        if (webp) {
            uint8_t *pixels = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(webp.get(), &pixels, &timestamp)) {
                *error = "Cannot decode WebP frame " + QString::number(i + 1);
                return false;
            }
            frame = QImage(pixels, int(info.canvas_width), int(info.canvas_height),
                           QImage::Format_RGBA8888);
            delay = qMax(1, timestamp - previousTimestamp);
            previousTimestamp = timestamp;
        } else {
            frame = reader.read();
            delay = qMax(1, reader.nextImageDelay());
        }
        if (frame.isNull()) {
            *error = "Cannot decode animation frame " + QString::number(i + 1) + ": " +
                     reader.errorString();
            return false;
        }
        QImage slide = background.copy();
        {
            QPainter p(&slide);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.save();
            p.scale(width / 1920.0, size.height() / 1080.0);
            QSizeF scaled = frame.size();
            scaled.scale(rect.size(),
                         media.span ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
            p.setClipRect(rect);
            p.drawImage(
                QRectF(rect.center() - QPointF(scaled.width() / 2, scaled.height() / 2), scaled),
                media.text.trimmed().isEmpty() ? frame : softenedImage(frame, scaled));
            p.restore();
            p.drawImage(0, 0, overlay);
        }
        duration += delay;
        const qint64 targetFrames =
            qMax<qint64>(i == frames - 1 ? 1 : 0, qRound64(duration * 60.0 / 1000));
        const QImage pixels = slide.convertToFormat(QImage::Format_RGBA8888);
        while (writtenFrames < targetFrames) {
            if (!writeFrame(pixels)) {
                *error = "Animation conversion failed: " +
                         QString::fromUtf8(diagnostics + encoder.readAllStandardError()).trimmed();
                return false;
            }
            ++writtenFrames;
        }
    }
    encoder.closeWriteChannel();
    do {
        encoder.waitForFinished(200);
        diagnostics = (diagnostics + encoder.readAllStandardError()).right(8192);
    } while (encoder.state() != QProcess::NotRunning);
    if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
        *error = "Animation conversion failed: " + QString::fromUtf8(diagnostics).trimmed();
        return false;
    }
    if (progress)
        progress(1);
    return true;
}
