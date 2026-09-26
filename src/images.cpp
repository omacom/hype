#include "images.h"
#include <QBuffer>
#include <QImageReader>
#include <QImageWriter>

QSize imageSizeForCanvas(QSize original, QSize canvas, bool span) {
    if (original.isEmpty() || canvas.isEmpty())
        return original;
    const QSize target = original.scaled(canvas, span ? Qt::KeepAspectRatioByExpanding
                                                      : Qt::KeepAspectRatio);
    return target.width() < original.width() ? target : original;
}

QImage readSizedImage(const QString &path, QSize canvas, bool span) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    QSize oriented = original;
    const bool rotated = reader.transformation() & QImageIOHandler::TransformationRotate90;
    if (rotated)
        oriented.transpose();
    // A vector image has no real size of its own: draw it at the size it will show, even when
    // that's larger than the size its file declares. Photos are never enlarged.
    const bool vector = reader.format().startsWith("svg");
    QSize target = vector && !canvas.isEmpty()
        ? oriented.scaled(canvas, span ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio)
        : imageSizeForCanvas(oriented, canvas, span);
    if (rotated)
        target.transpose();
    if (!target.isEmpty() && target != original)
        reader.setScaledSize(target);
    QImage image = reader.read();
    if (!image.isNull() && !vector) {
        const QSize size = imageSizeForCanvas(image.size(), canvas, span);
        if (size != image.size())
            image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

QByteArray compressedImage(const QImage &image, QString *extension) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    writer.setCompression(100); // Qt maps 0–100 to PNG's compression levels 0–9.
    if (!writer.write(image))
        return {};
    *extension = "png";
    // Tiny PNGs already compress well. For larger artwork, compare two lossless
    // encodings; never trade screenshot edges or transparency for smaller files.
    if (png.size() > 65536) {
        QByteArray webp;
        QBuffer webpBuffer(&webp);
        webpBuffer.open(QIODevice::WriteOnly);
        QImageWriter webpWriter(&webpBuffer, "webp");
        webpWriter.setQuality(100); // Qt's WebP writer selects lossless at 100.
        if (webpWriter.write(image) && webp.size() < png.size()) {
            *extension = "webp";
            return webp;
        }
    }
    return png;
}
