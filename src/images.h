#pragma once
#include <QImage>
#include <QByteArray>

QSize imageSizeForCanvas(QSize original, QSize canvas, bool span);
QImage readSizedImage(const QString &path, QSize canvas, bool span);
QByteArray compressedImage(const QImage &image, QString *extension);
// The formats a rendered slide can be saved in: "png", "webp" or "jpg", or nothing for a
// name that is none of them.
QByteArray pictureFormat(const QString &name);
// Saves a rendered slide. PNG keeps every pixel. WebP and JPEG give up a little for a file
// a fraction of the size, which is what matters when pictures cross a network; quality is
// 1 to 100, and WebP at 100 loses nothing.
bool savePicture(const QImage &image, const QString &path, const QByteArray &format, int quality);
