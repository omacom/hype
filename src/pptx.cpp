#include "pptx.h"
#include "budget.h"
#include "renderer.h"
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QXmlStreamWriter>
#include <cmath>
#include <functional>
#include <zlib.h>

namespace {
const QString pns = "http://schemas.openxmlformats.org/presentationml/2006/main";
const QString ans = "http://schemas.openxmlformats.org/drawingml/2006/main";
const QString rns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const QString packageNs = "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr qint64 slideWidth = 12192000, slideHeight = 6858000;
constexpr quint64 zipLimit = 0xffffffffULL;

// PNG and MP4 are already compressed. Stream each entry, then patch its CRC and
// length into the local header. Some office readers reject stored entries with
// data descriptors. Movies never need to be held in memory or read twice.
class Zip {
    struct Entry {
        QByteArray name;
        quint32 offset, size, crc;
    };
    QSaveFile output;
    QDataStream stream;
    QList<Entry> entries;

  public:
    QString error;
    explicit Zip(const QString &path) : output(path), stream(&output) {
        stream.setByteOrder(QDataStream::LittleEndian);
        if (!output.open(QIODevice::WriteOnly))
            error = output.errorString();
    }
    bool fail(const QString &message) {
        error = message;
        return false;
    }
    bool add(const QString &name, QIODevice &input) {
        if (!error.isEmpty())
            return false;
        if (entries.size() >= 65534 || quint64(output.pos()) >= zipLimit || input.size() < 0 ||
            quint64(input.size()) >= zipLimit ||
            quint64(output.pos()) + quint64(input.size()) + 65536 >= zipLimit)
            return fail("PowerPoint export exceeds the 4 GiB ZIP limit.");
        Entry entry{name.toUtf8(), quint32(output.pos()), 0, quint32(crc32(0, nullptr, 0))};
        stream << quint32(0x04034b50) << quint16(20) << quint16(0x800) << quint16(0) << quint16(0)
               << quint16(33) << quint32(0) << quint32(0) << quint32(0)
               << quint16(entry.name.size()) << quint16(0);
        if (output.write(entry.name) != entry.name.size())
            return fail(output.errorString());
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        while (true) {
            const qint64 count = input.read(buffer.data(), buffer.size());
            if (count < 0)
                return fail(input.errorString());
            if (!count)
                break;
            if (quint64(entry.size) + quint64(count) >= zipLimit ||
                quint64(output.pos()) + quint64(count) + 16 >= zipLimit)
                return fail("PowerPoint export exceeds the 4 GiB ZIP limit.");
            if (output.write(buffer.constData(), count) != count)
                return fail(output.errorString());
            entry.crc = quint32(
                crc32(entry.crc, reinterpret_cast<const Bytef *>(buffer.constData()), uInt(count)));
            entry.size += quint32(count);
        }
        const auto end = output.pos();
        if (!output.seek(qint64(entry.offset) + 14))
            return fail(output.errorString());
        stream << entry.crc << entry.size << entry.size;
        if (!output.seek(end))
            return fail(output.errorString());
        entries.append(entry);
        return stream.status() == QDataStream::Ok || fail(output.errorString());
    }
    bool file(const QString &name, const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return fail("Cannot read " + path + ": " + file.errorString());
        return add(name, file);
    }
    bool data(const QString &name, const QByteArray &bytes) {
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        return add(name, buffer);
    }
    bool finish() {
        if (!error.isEmpty())
            return false;
        const auto offset = output.pos();
        for (const auto &entry : entries) {
            stream << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0x800)
                   << quint16(0) << quint16(0) << quint16(33) << entry.crc << entry.size
                   << entry.size << quint16(entry.name.size()) << quint16(0) << quint16(0)
                   << quint16(0) << quint16(0) << quint32(0) << entry.offset;
            if (output.write(entry.name) != entry.name.size())
                return fail(output.errorString());
        }
        if (quint64(output.pos()) + 22 >= zipLimit)
            return fail("PowerPoint export exceeds the 4 GiB ZIP limit.");
        const auto directorySize = output.pos() - offset;
        stream << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(entries.size())
               << quint16(entries.size()) << quint32(directorySize) << quint32(offset)
               << quint16(0);
        if (stream.status() != QDataStream::Ok || !output.commit())
            return fail(output.errorString());
        return true;
    }
};

using Writer = QXmlStreamWriter;
using Attributes = QList<QPair<QString, QString>>;
void start(Writer &x, const QString &tag, const Attributes &attributes = {}) {
    x.writeStartElement(tag);
    for (const auto &attribute : attributes)
        x.writeAttribute(attribute.first, attribute.second);
}
void element(Writer &x, const QString &tag, const Attributes &attributes = {}) {
    start(x, tag, attributes);
    x.writeEndElement();
}
QByteArray xml(const std::function<void(Writer &)> &write) {
    QByteArray result;
    Writer x(&result);
    x.writeStartDocument();
    write(x);
    x.writeEndDocument();
    return result;
}
void presentationRoot(Writer &x, const QString &tag) {
    start(x, tag);
    x.writeNamespace(pns, "p");
    x.writeNamespace(ans, "a");
    x.writeNamespace(rns, "r");
}
struct Relationship {
    QString id, type, target;
};
QByteArray relationships(const QList<Relationship> &items) {
    return xml([&](Writer &x) {
        start(x, "Relationships");
        x.writeDefaultNamespace(packageNs);
        for (const auto &item : items)
            element(x, "Relationship",
                    {{"Id", item.id},
                     {"Type", item.type.contains(":") ? item.type : rns + "/" + item.type},
                     {"Target", item.target}});
        x.writeEndElement();
    });
}
void shapeTree(Writer &x) {
    start(x, "p:spTree");
    start(x, "p:nvGrpSpPr");
    element(x, "p:cNvPr", {{"id", "1"}, {"name", ""}});
    element(x, "p:cNvGrpSpPr");
    element(x, "p:nvPr");
    x.writeEndElement();
    element(x, "p:grpSpPr");
}
void masterColors(Writer &x) {
    start(x, "p:clrMapOvr");
    element(x, "a:masterClrMapping");
    x.writeEndElement();
}
struct PowerPointSlide {
    QString image, video, poster, overlay;
    bool autoplay = true, loop = false, muted = false;
    int repeatCount = 1;
    qint64 x = 0, y = 0, width = slideWidth, height = slideHeight;
};
void picture(Writer &x, int id, const QString &name, const QString &imageId,
             const PowerPointSlide &bounds, bool movie = false) {
    start(x, "p:pic");
    start(x, "p:nvPicPr");
    start(x, "p:cNvPr", {{"id", QString::number(id)}, {"name", name}});
    if (movie)
        element(x, "a:hlinkClick", {{"r:id", ""}, {"action", "ppaction://media"}});
    x.writeEndElement();
    start(x, "p:cNvPicPr");
    element(x, "a:picLocks", {{"noChangeAspect", "1"}});
    x.writeEndElement();
    start(x, "p:nvPr");
    if (movie) {
        element(x, "a:videoFile", {{"r:link", "rId4"}});
        start(x, "p:extLst");
        start(x, "p:ext", {{"uri", "{DAA4B4D4-6D71-4841-9C94-3DE7FCFB9230}"}});
        start(x, "p14:media", {{"r:embed", "rId3"}});
        x.writeNamespace("http://schemas.microsoft.com/office/powerpoint/2010/main", "p14");
        x.writeEndElement();
        x.writeEndElement();
        x.writeEndElement();
    }
    x.writeEndElement();
    x.writeEndElement();
    start(x, "p:blipFill");
    element(x, "a:blip", {{"r:embed", imageId}});
    start(x, "a:stretch");
    element(x, "a:fillRect");
    x.writeEndElement();
    x.writeEndElement();
    start(x, "p:spPr");
    start(x, "a:xfrm");
    element(x, "a:off", {{"x", QString::number(bounds.x)}, {"y", QString::number(bounds.y)}});
    element(x, "a:ext",
            {{"cx", QString::number(bounds.width)}, {"cy", QString::number(bounds.height)}});
    x.writeEndElement();
    start(x, "a:prstGeom", {{"prst", "rect"}});
    element(x, "a:avLst");
    x.writeEndElement();
    x.writeEndElement();
    x.writeEndElement();
}
QByteArray slideXml(const PowerPointSlide &slide) {
    return xml([&](Writer &x) {
        presentationRoot(x, "p:sld");
        start(x, "p:cSld");
        shapeTree(x);
        picture(x, 2, "PowerPointSlide", "rId2", PowerPointSlide{});
        if (!slide.video.isEmpty()) {
            picture(x, 3, QFileInfo(slide.video).fileName(), "rId5", slide, true);
            if (!slide.overlay.isEmpty())
                picture(x, 4, "Overlay", "rId6", PowerPointSlide{});
        }
        x.writeEndElement();
        x.writeEndElement();
        masterColors(x);
        if (!slide.video.isEmpty()) {
            start(x, "p:timing");
            start(x, "p:tnLst");
            start(x, "p:par");
            start(
                x, "p:cTn",
                {{"id", "1"}, {"dur", "indefinite"}, {"restart", "never"}, {"nodeType", "tmRoot"}});
            start(x, "p:childTnLst");
            start(x, "p:video");
            start(x, "p:cMediaNode", {{"vol", slide.muted ? "0" : "100000"}});
            start(x, "p:cTn", {{"id", "2"}, {"fill", "hold"}, {"display", "0"}});
            if (slide.loop)
                x.writeAttribute("repeatCount", "indefinite");
            else if (slide.repeatCount > 1)
                x.writeAttribute("repeatCount", QString::number(qint64(slide.repeatCount) * 1000));
            start(x, "p:stCondLst");
            element(x, "p:cond", {{"delay", slide.autoplay ? "0" : "indefinite"}});
            x.writeEndElement();
            x.writeEndElement();
            start(x, "p:tgtEl");
            element(x, "p:spTgt", {{"spid", "3"}});
            x.writeEndElement();
            for (int i = 0; i < 7; ++i)
                x.writeEndElement();
        }
        x.writeEndElement();
    });
}
QByteArray themeXml() {
    return xml([](Writer &x) {
        start(x, "a:theme", {{"name", "Hype"}});
        x.writeNamespace(ans, "a");
        start(x, "a:themeElements");
        start(x, "a:clrScheme", {{"name", "Hype"}});
        const QList<QPair<QString, QString>> colors = {
            {"dk1", "000000"},     {"lt1", "FFFFFF"},     {"dk2", "222222"},
            {"lt2", "EEEEEE"},     {"accent1", "4472C4"}, {"accent2", "ED7D31"},
            {"accent3", "A5A5A5"}, {"accent4", "FFC000"}, {"accent5", "5B9BD5"},
            {"accent6", "70AD47"}, {"hlink", "0563C1"},   {"folHlink", "954F72"}};
        for (const auto &color : colors) {
            start(x, "a:" + color.first);
            element(x, "a:srgbClr", {{"val", color.second}});
            x.writeEndElement();
        }
        x.writeEndElement();
        start(x, "a:fontScheme", {{"name", "Hype"}});
        for (const auto &type : {"a:majorFont", "a:minorFont"}) {
            start(x, type);
            element(x, "a:latin", {{"typeface", "Arial"}});
            element(x, "a:ea", {{"typeface", ""}});
            element(x, "a:cs", {{"typeface", ""}});
            x.writeEndElement();
        }
        x.writeEndElement();
        start(x, "a:fmtScheme", {{"name", "Hype"}});
        const auto fill = [&] {
            start(x, "a:solidFill");
            element(x, "a:schemeClr", {{"val", "phClr"}});
            x.writeEndElement();
        };
        start(x, "a:fillStyleLst");
        for (int i = 0; i < 3; ++i)
            fill();
        x.writeEndElement();
        start(x, "a:lnStyleLst");
        for (int i = 0; i < 3; ++i) {
            start(x, "a:ln", {{"w", QString::number(6350 * (i + 1))}});
            fill();
            element(x, "a:prstDash", {{"val", "solid"}});
            x.writeEndElement();
        }
        x.writeEndElement();
        start(x, "a:effectStyleLst");
        for (int i = 0; i < 3; ++i) {
            start(x, "a:effectStyle");
            element(x, "a:effectLst");
            x.writeEndElement();
        }
        x.writeEndElement();
        start(x, "a:bgFillStyleLst");
        for (int i = 0; i < 3; ++i)
            fill();
        x.writeEndElement();
        x.writeEndElement();
        x.writeEndElement();
        x.writeEndElement();
    });
}

bool readSlide(const QJsonObject &entry, const QDir &base, PowerPointSlide &slide, QString &error) {
    slide.image = base.filePath(entry["image"].toString());
    auto checkImage = [&](const QString &path) {
        // Check contents rather than relying on a filename extension.
        QImageReader reader(path);
        if (!reader.canRead() || reader.size().isEmpty()) {
            error = "Cannot read image: " + path;
            return false;
        }
        return true;
    };
    if (!checkImage(slide.image))
        return false;
    if (entry["video"].toString().isEmpty())
        return true;
    slide.video = base.filePath(entry["video"].toString());
    slide.poster = base.filePath(entry["poster"].toString());
    slide.autoplay = entry["autoplay"].toBool(true);
    slide.loop = entry["loop"].toBool();
    slide.repeatCount = entry["repeatCount"].toInt(1);
    slide.muted = entry["muted"].toBool();
    if (!entry["overlay_image"].toString().isEmpty())
        slide.overlay = base.filePath(entry["overlay_image"].toString());
    if (!checkImage(slide.poster) || (!slide.overlay.isEmpty() && !checkImage(slide.overlay)))
        return false;
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_streams", "-of", "json", slide.video});
    if (!probe.waitForStarted()) {
        error = "Cannot start ffprobe: " + probe.errorString();
        return false;
    }
    if (!probe.waitForFinished(30000)) {
        probe.kill();
        probe.waitForFinished();
        error = "Timed out inspecting video: " + slide.video;
        return false;
    }
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        error = "Cannot inspect video " + slide.video + ": " +
                QString::fromUtf8(probe.readAllStandardError()).trimmed();
        return false;
    }
    QJsonParseError jsonError;
    const auto info = QJsonDocument::fromJson(probe.readAllStandardOutput(), &jsonError);
    QJsonObject video, audio;
    for (const auto &value : info.object()["streams"].toArray()) {
        const auto stream = value.toObject();
        if (stream["codec_type"] == "video" && video.isEmpty())
            video = stream;
        if (stream["codec_type"] == "audio" && audio.isEmpty())
            audio = stream;
    }
    if (jsonError.error != QJsonParseError::NoError ||
        QFileInfo(slide.video).suffix().toLower() != "mp4" || video["codec_name"] != "h264" ||
        (!audio.isEmpty() && audio["codec_name"] != "aac") || video["width"].toInt() <= 0 ||
        video["height"].toInt() <= 0) {
        error = "PowerPoint video requires H.264/AAC MP4: " + QFileInfo(slide.video).fileName();
        return false;
    }
    const double ratio = double(video["width"].toInt()) / video["height"].toInt();
    if (entry["span"].toBool()) {
        if (std::abs(ratio - 16.0 / 9.0) > .01) {
            error =
                "Spanning video must be 16:9 for PowerPoint: " + QFileInfo(slide.video).fileName() +
                ". Use fit.";
            return false;
        }
    } else {
        Media media;
        media.video = true;
        media.text = entry["title"].toBool() ? "Title" : "";
        const QRectF box = mediaRect(media);
        const double boxX = box.x(), boxY = box.y();
        const double boxWidth = box.width(), boxHeight = box.height();
        const double width = std::min(boxWidth, boxHeight * ratio),
                     height = std::min(boxHeight, boxWidth / ratio);
        constexpr double scale = double(slideWidth) / 1920;
        slide.x = qint64((boxX + (boxWidth - width) / 2) * scale);
        slide.y = qint64((boxY + (boxHeight - height) / 2) * scale);
        slide.width = qint64(width * scale);
        slide.height = qint64(height * scale);
    }
    return true;
}
} // namespace

// Measured at 4K: one x264 thread needs 1.2 GB, each further one about 100 MB, and two
// threads encode twice as fast as one. Use half the cores at most, so the editor that
// launched this export stays responsive, and only what the free memory holds.
int encoderThreads() {
    return qMin(budget::workers(100LL << 20, 16, 0.6, 1200LL << 20), qMax(1, QThread::idealThreadCount() / 2));
}
QString preparePowerPointVideo(const QString &source, const QString &output, QString *error,
                              const std::function<void(double)> &progress) {
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", source});
    if (!probe.waitForFinished(30000) || probe.exitCode() != 0) {
        probe.kill();
        probe.waitForFinished();
        *error = "Cannot inspect video: " + QFileInfo(source).fileName();
        return {};
    }
    const auto info = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
    QJsonObject video, audio;
    for (const auto &value : info["streams"].toArray()) {
        const auto stream = value.toObject();
        if (stream["codec_type"] == "video" && video.isEmpty()) video = stream;
        if (stream["codec_type"] == "audio" && audio.isEmpty()) audio = stream;
    }
    if (video.isEmpty()) {
        *error = "No video stream in " + QFileInfo(source).fileName();
        return {};
    }
    if (QFileInfo(source).suffix().toLower() == "mp4" && video["codec_name"] == "h264" &&
        (audio.isEmpty() || audio["codec_name"] == "aac"))
        return source;
    const double duration = info["format"].toObject()["duration"].toString().toDouble();
    QProcess encoder;
    encoder.start("ffmpeg", {"-v", "error", "-nostdin", "-y", "-i", source,
        "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "libx264", "-preset", "fast", "-crf", "18",
        "-threads", QString::number(encoderThreads()), "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", "-progress", "pipe:1", output});
    if (!encoder.waitForStarted()) {
        *error = "Cannot start video conversion: " + encoder.errorString();
        return {};
    }
    QByteArray pending, errors;
    do {
        encoder.waitForFinished(200);
        errors = (errors + encoder.readAllStandardError()).right(8192);
        pending += encoder.readAllStandardOutput();
        int end;
        while ((end = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(end);
            pending.remove(0, end + 1);
            if (progress && duration > 0 && line.startsWith("out_time_us="))
                progress(qBound(0.0, line.mid(12).toDouble() / 1e6 / duration, 1.0));
        }
    } while (encoder.state() != QProcess::NotRunning);
    if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
        *error = "Cannot convert " + QFileInfo(source).fileName() + ": " + QString::fromUtf8(errors).trimmed();
        return {};
    }
    return output;
}

bool writePptx(const QString &manifestPath, const QString &destination, QString *error,
               const std::function<void(double)> &progress) {
    if (error)
        error->clear();
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly))
        return fail("Cannot read slide manifest: " + manifest.errorString());
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object()["slides"].isArray())
        return fail("Invalid slide manifest: " + parseError.errorString());
    QList<PowerPointSlide> slides;
    const auto deck = document.object();
    for (const auto &value : deck["slides"].toArray()) {
        PowerPointSlide slide;
        QString problem;
        if (!value.isObject())
            return fail("Invalid slide in manifest.");
        if (!readSlide(value.toObject(), QFileInfo(manifestPath).absoluteDir(), slide, problem))
            return fail(problem);
        slides.append(slide);
    }
    if (slides.isEmpty())
        return fail("There are no slides to export.");
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return fail("Cannot create the export directory.");
    Zip zip(destination);
    if (!zip.error.isEmpty())
        return fail(zip.error);
    auto put = [&](const QString &path, const QByteArray &bytes) { return zip.data(path, bytes); };
    put("_rels/.rels", relationships({{"rId1", "officeDocument", "ppt/presentation.xml"},
                                      {"rId2",
                                       "http://schemas.openxmlformats.org/package/2006/"
                                       "relationships/metadata/core-properties",
                                       "docProps/core.xml"}}));
    put("docProps/core.xml", xml([&](Writer &x) {
            start(x, "cp:coreProperties");
            x.writeNamespace(
                "http://schemas.openxmlformats.org/package/2006/metadata/core-properties", "cp");
            x.writeNamespace("http://purl.org/dc/elements/1.1/", "dc");
            x.writeTextElement("dc:title", deck["title"].toString());
            x.writeTextElement("dc:creator", "Hype");
            x.writeEndElement();
        }));
    put("ppt/presentation.xml", xml([&](Writer &x) {
            presentationRoot(x, "p:presentation");
            x.writeAttribute("autoCompressPictures", "0");
            start(x, "p:sldMasterIdLst");
            element(x, "p:sldMasterId", {{"id", "2147483648"}, {"r:id", "rId1"}});
            x.writeEndElement();
            start(x, "p:sldIdLst");
            for (int i = 0; i < slides.size(); ++i)
                element(
                    x, "p:sldId",
                    {{"id", QString::number(256 + i)}, {"r:id", "rId" + QString::number(i + 2)}});
            x.writeEndElement();
            element(x, "p:sldSz",
                    {{"cx", QString::number(slideWidth)},
                     {"cy", QString::number(slideHeight)},
                     {"type", "screen16x9"}});
            element(x, "p:notesSz", {{"cx", "6858000"}, {"cy", "9144000"}});
            x.writeEndElement();
        }));
    QList<Relationship> presentationRels{{"rId1", "slideMaster", "slideMasters/slideMaster1.xml"}};
    for (int i = 0; i < slides.size(); ++i)
        presentationRels.append({"rId" + QString::number(i + 2), "slide",
                                 "slides/slide" + QString::number(i + 1) + ".xml"});
    put("ppt/_rels/presentation.xml.rels", relationships(presentationRels));
    put("ppt/slideMasters/slideMaster1.xml", xml([](Writer &x) {
            presentationRoot(x, "p:sldMaster");
            start(x, "p:cSld");
            shapeTree(x);
            x.writeEndElement();
            x.writeEndElement();
            element(x, "p:clrMap",
                    {{"bg1", "lt1"},
                     {"tx1", "dk1"},
                     {"bg2", "lt2"},
                     {"tx2", "dk2"},
                     {"accent1", "accent1"},
                     {"accent2", "accent2"},
                     {"accent3", "accent3"},
                     {"accent4", "accent4"},
                     {"accent5", "accent5"},
                     {"accent6", "accent6"},
                     {"hlink", "hlink"},
                     {"folHlink", "folHlink"}});
            start(x, "p:sldLayoutIdLst");
            element(x, "p:sldLayoutId", {{"id", "2147483649"}, {"r:id", "rId1"}});
            x.writeEndElement();
            x.writeEndElement();
        }));
    put("ppt/slideMasters/_rels/slideMaster1.xml.rels",
        relationships({{"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"},
                       {"rId2", "theme", "../theme/theme1.xml"}}));
    put("ppt/slideLayouts/slideLayout1.xml", xml([](Writer &x) {
            presentationRoot(x, "p:sldLayout");
            x.writeAttribute("type", "blank");
            x.writeAttribute("preserve", "1");
            start(x, "p:cSld", {{"name", "Blank"}});
            shapeTree(x);
            x.writeEndElement();
            x.writeEndElement();
            masterColors(x);
            x.writeEndElement();
        }));
    put("ppt/slideLayouts/_rels/slideLayout1.xml.rels",
        relationships({{"rId1", "slideMaster", "../slideMasters/slideMaster1.xml"}}));
    put("ppt/theme/theme1.xml", themeXml());
    auto media = [&](const QString &path, const QString &name) {
        QImageReader reader(path);
        const QByteArray format = reader.format();
        const QString suffix = format == "jpeg" ? ".jpg" : ".png";
        const QString target = "media/" + name + suffix;
        if (format == "png" || format == "jpeg") {
            zip.file("ppt/" + target, path);
        } else {
            // Normalize other supported poster formats for PowerPoint.
            QBuffer encoded;
            encoded.open(QIODevice::WriteOnly);
            if (!reader.read().save(&encoded, "PNG"))
                zip.fail("Cannot encode image for PowerPoint: " + path);
            else
                zip.data("ppt/" + target, encoded.data());
        }
        return "../" + target;
    };
    QHash<QString, QString> movies;
    for (int i = 0; i < slides.size() && zip.error.isEmpty(); ++i) {
        if (progress) progress(double(i) / slides.size());
        const auto &slide = slides[i];
        const QString number = QString::number(i + 1);
        QList<Relationship> rels{{"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"},
                                 {"rId2", "image", media(slide.image, "slide" + number)}};
        if (!slide.video.isEmpty()) {
            const QString identity = QFileInfo(slide.video).canonicalFilePath();
            QString movie = movies.value(identity);
            if (movie.isEmpty()) {
                movie = "media/video" + number + ".mp4";
                zip.file("ppt/" + movie, slide.video);
                movies.insert(identity, movie);
            }
            rels.append({"rId3", "http://schemas.microsoft.com/office/2007/relationships/media",
                         "../" + movie});
            rels.append({"rId4", "video", "../" + movie});
            rels.append({"rId5", "image", media(slide.poster, "poster" + number)});
            if (!slide.overlay.isEmpty())
                rels.append({"rId6", "image", media(slide.overlay, "overlay" + number)});
        }
        put("ppt/slides/slide" + number + ".xml", slideXml(slide));
        put("ppt/slides/_rels/slide" + number + ".xml.rels", relationships(rels));
    }
    put("[Content_Types].xml", xml([&](Writer &x) {
            start(x, "Types");
            x.writeDefaultNamespace("http://schemas.openxmlformats.org/package/2006/content-types");
            for (const auto &type : QList<QPair<QString, QString>>{
                     {"rels", "application/vnd.openxmlformats-package.relationships+xml"},
                     {"xml", "application/xml"},
                     {"png", "image/png"},
                     {"jpg", "image/jpeg"},
                     {"mp4", "video/mp4"}})
                element(x, "Default", {{"Extension", type.first}, {"ContentType", type.second}});
            auto part = [&](const QString &name, const QString &type) {
                element(x, "Override", {{"PartName", name}, {"ContentType", type}});
            };
            const QString prefix = "application/vnd.openxmlformats-officedocument.";
            part("/docProps/core.xml",
                 "application/vnd.openxmlformats-package.core-properties+xml");
            part("/ppt/presentation.xml", prefix + "presentationml.presentation.main+xml");
            part("/ppt/slideMasters/slideMaster1.xml", prefix + "presentationml.slideMaster+xml");
            part("/ppt/slideLayouts/slideLayout1.xml", prefix + "presentationml.slideLayout+xml");
            part("/ppt/theme/theme1.xml", prefix + "theme+xml");
            for (int i = 0; i < slides.size(); ++i)
                part("/ppt/slides/slide" + QString::number(i + 1) + ".xml",
                     prefix + "presentationml.slide+xml");
            x.writeEndElement();
        }));
    return zip.finish() || fail(zip.error);
}
