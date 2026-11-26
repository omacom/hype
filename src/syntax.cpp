#include "syntax.h"
#include <QCache>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QWaitCondition>

static QString highlightedHtml(const QString &source, QString language) {
    language = language.toLower().section(' ', 0, 0);
    static const QMap<QString, QString> aliases{
        {"shell", "sh"}, {"shellscript", "sh"}, {"c++", "cpp"}, {"yml", "yaml"},
        {"rust", "rs"}};
    language = aliases.value(language, language);
    static thread_local const QRegularExpression validLanguage("^[a-z0-9+#_-]+$");
    if (!validLanguage.match(language).hasMatch())
        return {};
    static QCache<QString, QString> cache(4 * 1024 * 1024);
    static QMutex mutex;
    static QWaitCondition ready;
    static QSet<QString> pending;
    const QString key = language + '\n' + source;
    {
        QMutexLocker lock(&mutex);
        while (pending.contains(key))
            ready.wait(&mutex);
        if (auto html = cache.object(key))
            return *html;
        pending.insert(key);
    }
    QProcess process;
    process.start("source-highlight", {"--src-lang=" + language, "--out-format=html-css"});
    QString html;
    if (process.waitForStarted(1000)) {
        process.write(source.toUtf8());
        process.closeWriteChannel();
        if (process.waitForFinished(3000) && process.exitStatus() == QProcess::NormalExit &&
            process.exitCode() == 0)
            html = QString::fromUtf8(process.readAllStandardOutput());
        else {
            process.kill();
            process.waitForFinished();
        }
    }
    {
        QMutexLocker lock(&mutex);
        cache.insert(key, new QString(html), qMax(1, int((key.size() + html.size()) * 2)));
        pending.remove(key);
        ready.wakeAll();
    }
    return html;
}

void highlightCode(QTextDocument &document, const QVariantMap &palette) {
    QString css;
    const QMap<QString, QString> colors{
        {"keyword", "magenta"},  {"type", "yellow"},     {"classname", "yellow"},
        {"string", "green"},     {"regexp", "green"},    {"specialchar", "cyan"},
        {"number", "red"},       {"function", "accent"}, {"preproc", "cyan"},
        {"symbol", "cyan"},      {"variable", "red"},    {"comment", "dark_foreground"},
        {"normal", "foreground"}};
    for (auto it = colors.cbegin(); it != colors.cend(); ++it)
        css +=
            QString("span.%1 { color: %2; }")
                .arg(it.key(), palette.value(it.value(), palette.value("foreground")).toString());
    for (QTextBlock block = document.begin(); block.isValid();) {
        const QString language = block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
        if (language.isEmpty()) {
            block = block.next();
            continue;
        }
        const int start = block.position();
        QString source = block.text();
        block = block.next();
        while (block.isValid() &&
               block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage) == language) {
            source += '\n' + block.text();
            block = block.next();
        }
        const QString html = highlightedHtml(source, language);
        if (html.isEmpty())
            continue;
        QTextDocument highlighted;
        highlighted.setDefaultStyleSheet(css);
        highlighted.setHtml(html.trimmed());
        // Refuse to apply offsets if the formatter changed whitespace or Unicode.
        if (highlighted.toPlainText() != source && highlighted.toPlainText() != source + '\n')
            continue;
        for (auto b = highlighted.begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.isValid() || fragment.position() >= source.size())
                    continue;
                QTextCursor cursor(&document);
                cursor.setPosition(start + fragment.position());
                cursor.setPosition(
                    start + qMin(int(source.size()), fragment.position() + fragment.length()),
                    QTextCursor::KeepAnchor);
                QTextCharFormat format;
                format.setForeground(fragment.charFormat().foreground());
                cursor.mergeCharFormat(format);
            }
        }
    }
}
