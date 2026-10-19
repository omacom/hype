// Lucide icons (ISC, see icons-LICENSE.txt), packed one per line in icons.tsv:
// name, SVG elements on a 24×24 grid (or @canonical for an alias), search tags.
#include "blocks.h"
#include "blocks_p.h"
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <algorithm>

namespace {
struct Icon {
    QByteArray body;
    QString tags, alias;
};
const QHash<QString, Icon> &icons() {
    static const QHash<QString, Icon> table = [] {
        QHash<QString, Icon> result;
        QFile file(":/icons.tsv");
        if (!file.open(QIODevice::ReadOnly))
            return result;
        for (const QByteArray &line : file.readAll().split('\n')) {
            const auto fields = line.split('\t');
            if (fields.size() < 2 || fields[0].isEmpty())
                continue;
            Icon icon;
            if (fields[1].startsWith('@'))
                icon.alias = QString::fromUtf8(fields[1].mid(1));
            else
                icon.body = fields[1];
            if (fields.size() > 2)
                icon.tags = QString::fromUtf8(fields[2]);
            result.insert(QString::fromUtf8(fields[0]), icon);
        }
        return result;
    }();
    return table;
}
const Icon *find(const QString &name) {
    const auto &table = icons();
    auto it = table.constFind(name.trimmed().toLower());
    if (it == table.cend())
        return nullptr;
    if (!it->alias.isEmpty())
        it = table.constFind(it->alias);
    return it == table.cend() ? nullptr : &*it;
}
int distance(const QString &a, const QString &b) {
    QVector<int> row(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) row[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        int previous = row[0];
        row[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int current = row[j];
            row[j] = qMin(qMin(row[j] + 1, row[j - 1] + 1), previous + (a[i - 1] == b[j - 1] ? 0 : 1));
            previous = current;
        }
    }
    return row[b.size()];
}
}

bool iconExists(const QString &name) { return find(name); }

QStringList iconSuggestions(const QString &name) {
    // Words in the unknown name are usually tags or parts of the right name.
    QStringList found = searchIcons(QString(name).replace('-', ' '), 3);
    if (found.size() < 3) {
        QVector<QPair<int, QString>> near;
        for (auto it = icons().cbegin(); it != icons().cend(); ++it)
            near.append({distance(name, it.key()), it.key()});
        std::sort(near.begin(), near.end());
        for (int i = 0; i < near.size() && found.size() < 3; ++i)
            if (!found.contains(near[i].second)) found << near[i].second;
    }
    return found;
}

QStringList searchIcons(const QString &query, int limit) {
    const QStringList words = query.toLower().split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
    QVector<QPair<int, QString>> scored;
    for (auto it = icons().cbegin(); it != icons().cend(); ++it) {
        if (!it->alias.isEmpty() && !words.isEmpty())
            continue; // Search canonical names; aliases still resolve when used.
        int score = 0;
        bool all = true;
        for (const QString &word : words) {
            const QStringList parts = it.key().split('-');
            if (it.key() == word) score += 100;
            else if (parts.contains(word)) score += 40;
            else if (it.key().contains(word)) score += 20;
            else if (it->tags.split(' ').contains(word)) score += 10;
            else if (it->tags.contains(word)) score += 4;
            else { all = false; break; }
        }
        if (all)
            scored.append({-score * 1000 + int(it.key().size()), it.key()});
    }
    std::sort(scored.begin(), scored.end());
    QStringList names;
    for (const auto &entry : scored) {
        if (limit > 0 && names.size() >= limit) break;
        names << entry.second;
    }
    return names;
}

void paintIcon(QPainter *p, const QString &name, const QRectF &rect, const QColor &color, qreal stroke) {
    const Icon *icon = find(name);
    if (!icon || rect.isEmpty())
        return;
    const QByteArray svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"" +
        color.name(QColor::HexRgb).toLatin1() + "\" stroke-opacity=\"" + QByteArray::number(color.alphaF()) +
        "\" stroke-width=\"" + QByteArray::number(stroke) +
        "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" + icon->body + "</svg>";
    QSvgRenderer renderer(svg);
    const qreal side = qMin(rect.width(), rect.height());
    renderer.render(p, QRectF(rect.center() - QPointF(side / 2, side / 2), QSizeF(side, side)));
}
