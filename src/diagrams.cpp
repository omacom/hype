// Components: charts and diagrams drawn from a Markdown table or list.
#include "blocks_p.h"
#include "qrcodegen.h"
#include "renderer.h"
#include <QDate>
#include <QPainter>
#include <QRegularExpression>
#include <QTextDocument>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <cmath>
#include <functional>

namespace {

const QStringList diagrams{"Chart", "Progress", "Gantt", "Timeline", "Steps", "Cycle", "Funnel", "Venn",
                           "Tree",  "QR",       "Collage", "Video"};

QStringList colorList(const Node &n) {
    return n.prop("colors").split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
}
QColor toneOr(const Node &n, const Ctx &c, const QString &fallback = "accent") {
    const QColor color = c.tone(n.prop("tone", fallback), n.ink);
    return color.isValid() ? color : n.ink.accent;
}
qreal scaleOf(const Node &n) {
    // size="sm|md|lg" scales a diagram's type relative to the slide body.
    const QString v = n.prop("size", "md");
    return v == "xs" ? 0.75 : v == "sm" ? 0.87 : v == "lg" ? 1.18 : v == "xl" ? 1.35 : 1.0;
}

QSharedPointer<QTextDocument> words(const QString &markdown, qreal size, const QColor &ink, const QColor &accent,
                                    Qt::Alignment align, Ctx &c, bool heading = false, int weight = -1,
                                    qreal line = 1.25) {
    TextStyle st;
    st.size = size;
    st.ink = ink;
    st.accent = accent;
    st.align = align;
    st.heading = heading;
    st.weight = weight;
    st.lineHeight = line;
    return makeText(markdown, st, c);
}
qreal heightAt(QTextDocument &doc, qreal width) {
    doc.setTextWidth(qMax(1.0, width));
    return doc.size().height();
}
// The height at a width, noting a word too long for it, so the slide shrinks instead.
qreal fitted(QTextDocument &doc, qreal width, Ctx &c) {
    const qreal h = heightAt(doc, width);
    if (doc.size().width() > width + 1) c.overflow = true;
    return h;
}

// An item's title, a first line in bold and any remaining text, as one block.
struct ItemText {
    QSharedPointer<QTextDocument> title, body;
    Ctx *c = nullptr;
    qreal height(qreal width, qreal gap) const {
        qreal h = 0;
        if (title) h += c ? fitted(*title, width, *c) : heightAt(*title, width);
        if (body) h += (title ? gap : 0) + (c ? fitted(*body, width, *c) : heightAt(*body, width));
        return h;
    }
    void paint(QPainter *p, QPointF at, qreal width, qreal gap) const {
        if (title) {
            const qreal h = heightAt(*title, width);
            drawText(*title, at, p);
            at.ry() += h + gap;
        }
        if (body) {
            heightAt(*body, width);
            drawText(*body, at, p);
        }
    }
};
ItemText itemText(const Item &item, const Node &n, Ctx &c, Qt::Alignment align, qreal titleSize, qreal bodySize,
                  const QColor &titleColor = QColor()) {
    ItemText t;
    t.c = &c;
    if (!item.title.isEmpty())
        t.title = words(item.title, titleSize, titleColor.isValid() ? titleColor : n.ink.fg, n.ink.accent, align, c, true);
    if (!item.text.isEmpty())
        t.body = words(item.text, bodySize, n.ink.muted.isValid() ? mix(n.ink.fg, n.ink.bg, 0.2) : n.ink.fg,
                       n.ink.accent, align, c);
    return t;
}

// ---------------------------------------------------------------------------
// Chart

struct Series {
    QString name;
    QVector<double> values;
    QStringList labels;
};
struct ChartData {
    QStringList categories;
    QVector<Series> series;
    QString error;
};
ChartData chartData(const Node &n) {
    ChartData d;
    const Table table = parseTable(n.text);
    if (table.header.size() < 2 || table.rows.isEmpty()) {
        d.error = "needs a Markdown table: a header row, then one row per category with a value per series";
        return d;
    }
    for (int col = 1; col < table.header.size(); ++col) d.series.append({plainText(table.header[col]), {}, {}});
    for (const QStringList &row : table.rows) {
        d.categories << plainText(row.value(0));
        for (int col = 1; col < table.header.size(); ++col) {
            bool ok = false;
            const QString cell = row.value(col);
            const double value = cell.trimmed().isEmpty() ? 0 : parseNumber(cell, &ok);
            if (!ok && !cell.trimmed().isEmpty() && d.error.isEmpty())
                d.error = "can't read “" + cell + "” as a number";
            d.series[col - 1].values << value;
            d.series[col - 1].labels << plainText(cell);
        }
    }
    return d;
}

double niceStep(double range, int ticks) {
    const double rough = range / qMax(1, ticks);
    const double magnitude = std::pow(10, std::floor(std::log10(qMax(1e-12, rough))));
    const double residual = rough / magnitude;
    return (residual <= 1 ? 1 : residual <= 2 ? 2 : residual <= 2.5 ? 2.5 : residual <= 5 ? 5 : 10) * magnitude;
}
QString compact(double value) {
    const double a = std::abs(value);
    auto trim = [](double v) {
        QString s = QString::number(v, 'f', v < 10 ? 1 : 0);
        if (s.endsWith(".0")) s.chop(2);
        return s;
    };
    if (a >= 1e9) return trim(value / 1e9) + "B";
    if (a >= 1e6) return trim(value / 1e6) + "M";
    if (a >= 1e4) return trim(value / 1e3) + "K";
    QString s = QString::number(value, 'f', a < 10 && std::fmod(a, 1) != 0 ? 1 : 0);
    if (a >= 1000) s = QLocale(QLocale::English).toString(qRound64(value));
    return s;
}

void paintLegend(QPainter *p, const QRectF &area, const ChartData &d, const QVector<QColor> &colors, const QFont &font,
                 const QColor &ink, bool line) {
    const QFontMetricsF metrics(font);
    qreal x = area.left();
    const qreal swatch = metrics.height() * 0.62;
    p->setFont(font);
    for (int i = 0; i < d.series.size(); ++i) {
        const qreal w = metrics.horizontalAdvance(d.series[i].name);
        if (line) p->fillRect(QRectF(x, area.center().y() - swatch * 0.12, swatch * 1.6, swatch * 0.24), colors[i]);
        else {
            p->setPen(Qt::NoPen);
            p->setBrush(colors[i]);
            p->drawRoundedRect(QRectF(x, area.center().y() - swatch / 2, swatch, swatch), swatch * 0.25, swatch * 0.25);
        }
        x += swatch * (line ? 1.6 : 1) + swatch * 0.6;
        p->setPen(ink);
        p->drawText(QRectF(x, area.top(), w + 2, area.height()), Qt::AlignVCenter | Qt::AlignLeft, d.series[i].name);
        x += w + swatch * 1.8;
    }
}

void paintChart(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const ChartData d = chartData(n);
    if (!d.error.isEmpty() || d.series.isEmpty()) return;
    const QString type = n.prop("type", "column");
    const qreal k = scaleOf(n);
    const QFont label = uiFont(c, c.px(19 * k));
    const QFont value = uiFont(c, c.px(20 * k), false, 700);
    const QFontMetricsF lm(label), vm(value);
    const QColor ink = n.ink.fg, muted = mix(n.ink.fg, n.ink.bg, 0.35), grid = mix(n.ink.bg, n.ink.fg, 0.12);
    QVector<QColor> colors;
    for (int i = 0; i < d.series.size(); ++i) colors << seriesColor(i, n.ink, c, colorList(n));
    QRectF area = n.rect;
    const bool many = d.series.size() > 1;
    const QString legend = n.prop("legend", "auto");
    const bool pie = type == "pie" || type == "donut";
    const bool inlineLegend = legend == "inline" && (type == "line" || type == "area");
    if (many && legend != "none" && !pie && !inlineLegend) {
        paintLegend(p, QRectF(area.left(), area.top(), area.width(), lm.height() * 1.4), d, colors, label, ink,
                    type == "line");
        area.setTop(area.top() + lm.height() * 2.1);
    }
    p->setRenderHint(QPainter::Antialiasing);
    const QString labels = n.prop("labels", "auto");

    if (pie) {
        const QVector<double> &values = d.series[0].values;
        double total = 0;
        for (double v : values) total += qMax(0.0, v);
        if (total <= 0) return;
        const qreal legendWidth = qMin(area.width() * 0.45, c.px(420 * k));
        const qreal side = qMin(area.height(), area.width() - legendWidth - c.px(40));
        const QRectF circle(area.left() + (area.width() - legendWidth - c.px(40) - side) / 2, area.center().y() - side / 2,
                            side, side);
        QVector<QColor> slices;
        for (int i = 0; i < values.size(); ++i) slices << seriesColor(i, n.ink, c, colorList(n));
        double angle = 90 * 16;
        for (int i = 0; i < values.size(); ++i) {
            const double span = -qMax(0.0, values[i]) / total * 360 * 16;
            QPainterPath slice;
            slice.moveTo(circle.center());
            slice.arcTo(circle, angle / 16, span / 16);
            slice.closeSubpath();
            p->fillPath(slice, slices[i]);
            p->setPen(QPen(n.ink.bg, c.px(3)));
            p->setBrush(Qt::NoBrush);
            p->drawPath(slice);
            angle += span;
        }
        if (type == "donut") {
            p->setPen(Qt::NoPen);
            p->setBrush(n.ink.bg);
            p->drawEllipse(circle.center(), side * 0.31, side * 0.31);
        }
        // Legend with values: identity never rides on color alone.
        const qreal rowH = lm.height() * 1.7;
        qreal y = area.center().y() - rowH * values.size() / 2;
        const qreal x = circle.right() + c.px(48);
        for (int i = 0; i < values.size(); ++i) {
            const qreal sw = lm.height() * 0.62;
            p->setPen(Qt::NoPen);
            p->setBrush(slices[i]);
            p->drawRoundedRect(QRectF(x, y + (rowH - sw) / 2, sw, sw), sw * 0.25, sw * 0.25);
            const QString share = QString::number(qRound(values[i] / total * 100)) + "%";
            drawLabel(p, QRectF(x + sw * 1.7, y, legendWidth - sw * 1.7 - vm.horizontalAdvance("100%") - c.px(12), rowH),
                      d.categories.value(i), label, ink, Qt::AlignVCenter | Qt::AlignLeft);
            drawLabel(p, QRectF(area.right() - vm.horizontalAdvance("100%") - c.px(4), y, vm.horizontalAdvance("100%"), rowH),
                      share, value, ink, Qt::AlignVCenter | Qt::AlignRight);
            y += rowH;
        }
        return;
    }

    const bool horizontal = type == "bar" || type == "stacked-bar";
    const bool stacked = type.startsWith("stacked");
    const bool line = type == "line" || type == "area";
    double low = 0, high = 0;
    for (int ci = 0; ci < d.categories.size(); ++ci) {
        double positive = 0, negative = 0;
        for (const Series &s : d.series) {
            const double v = s.values.value(ci);
            if (stacked) (v >= 0 ? positive : negative) += v;
            else {
                high = qMax(high, v);
                low = qMin(low, v);
            }
        }
        if (stacked) {
            high = qMax(high, positive);
            low = qMin(low, negative);
        }
    }
    bool ok = false;
    const double goal = n.prop("goal").isEmpty() ? NAN : parseNumber(n.prop("goal"), &ok);
    if (ok) high = qMax(high, goal);
    if (n.has("max")) high = parseNumber(n.prop("max"));
    if (n.has("min")) low = parseNumber(n.prop("min"));
    if (line && !n.has("min") && low >= 0) {
        // Lines may start above zero; give them the room their changes need.
        double minimum = high;
        for (const Series &s : d.series)
            for (double v : s.values) minimum = qMin(minimum, v);
        if (minimum > high * 0.35) low = minimum - (high - minimum) * 0.25;
    }
    if (high <= low) high = low + 1;
    const double step = niceStep(high - low, 4);
    high = std::ceil(high / step - 1e-9) * step;
    low = std::floor(low / step + 1e-9) * step;

    QStringList ticks;
    for (double t = low; t <= high + step / 2; t += step) ticks << compact(t);
    qreal axisWidth = 0;
    for (const QString &t : ticks) axisWidth = qMax(axisWidth, lm.horizontalAdvance(t));
    axisWidth += c.px(14);
    qreal categoryWidth = 0;
    for (const QString &cat : d.categories) categoryWidth = qMax(categoryWidth, lm.horizontalAdvance(cat));
    QRectF plot = area;
    if (horizontal) {
        plot.setLeft(area.left() + qMin(categoryWidth + c.px(18), area.width() * 0.32));
        plot.setBottom(area.bottom() - lm.height() * 1.5);
        plot.setRight(area.right() - vm.horizontalAdvance("0000") * 0.8);
    } else {
        plot.setLeft(area.left() + axisWidth);
        plot.setBottom(area.bottom() - lm.height() * 1.6);
        plot.setTop(plot.top() + vm.height() * 1.3);
        if (line) plot.setRight(area.right() - vm.horizontalAdvance("00000"));
    }
    // Marks: `Q2 26: Pilot ends; Q4 26: Rollout complete` flag categories with a labelled line.
    struct Mark {
        int category;
        QString label;
    };
    QVector<Mark> marks;
    for (const QString &entry : n.prop("marks").split(';', Qt::SkipEmptyParts)) {
        const int colon = entry.indexOf(':');
        const QString key = (colon < 0 ? entry : entry.left(colon)).trimmed();
        const int index = d.categories.indexOf(QRegularExpression("^" + QRegularExpression::escape(key) + "$",
                                                                  QRegularExpression::CaseInsensitiveOption));
        if (index >= 0) marks.append({index, colon < 0 ? QString() : entry.mid(colon + 1).trimmed()});
    }
    if (!marks.isEmpty() && !horizontal) plot.setTop(plot.top() + vm.height() * 1.6);
    auto toAxis = [&](double v) {
        const double t = (v - low) / (high - low);
        return horizontal ? plot.left() + t * plot.width() : plot.bottom() - t * plot.height();
    };
    // Gridlines and tick labels: hairline, solid, recessive.
    p->setFont(label);
    for (int i = 0; i < ticks.size(); ++i) {
        const double v = low + i * step;
        const qreal at = toAxis(v);
        p->setPen(QPen(v == 0 ? mix(n.ink.bg, n.ink.fg, 0.3) : grid, c.px(v == 0 ? 2 : 1.2)));
        if (horizontal) {
            p->drawLine(QPointF(at, plot.top()), QPointF(at, plot.bottom()));
            p->setPen(muted);
            p->drawText(QRectF(at - 60, plot.bottom() + c.px(6), 120, lm.height()), Qt::AlignHCenter | Qt::AlignTop, ticks[i]);
        } else {
            p->drawLine(QPointF(plot.left(), at), QPointF(plot.right(), at));
            p->setPen(muted);
            p->drawText(QRectF(area.left(), at - lm.height() / 2, axisWidth - c.px(12), lm.height()),
                        Qt::AlignRight | Qt::AlignVCenter, ticks[i]);
        }
    }
    const int count = d.categories.size();
    const qreal band = (horizontal ? plot.height() : plot.width()) / qMax(1, count);
    const QString highlight = n.prop("highlight");
    const qreal radius = c.px(6);
    auto barPath = [&](QRectF r, bool positive) {
        // Rounded at the data end, square at the baseline.
        if (horizontal)
            return positive ? roundedPath(r, 0, radius, radius, 0) : roundedPath(r, radius, 0, 0, radius);
        return positive ? roundedPath(r, radius, radius, 0, 0) : roundedPath(r, 0, 0, radius, radius);
    };
    const int bars = stacked ? 1 : d.series.size();
    const bool showValues = labels == "all" || (labels == "auto" && count * bars <= 16);
    for (int ci = 0; ci < count; ++ci) {
        const qreal start = (horizontal ? plot.top() : plot.left()) + ci * band;
        // Category label
        if (horizontal)
            drawLabel(p, QRectF(area.left(), start, plot.left() - area.left() - c.px(16), band), d.categories[ci], label, ink,
                      Qt::AlignRight | Qt::AlignVCenter);
        else
            drawLabel(p, QRectF(start, plot.bottom() + c.px(8), band, lm.height() * 1.3), d.categories[ci], label, ink,
                      Qt::AlignHCenter | Qt::AlignTop);
        if (line) continue;
        const bool dim = !highlight.isEmpty() && highlight.compare(d.categories[ci], Qt::CaseInsensitive) != 0;
        const qreal thickness = qMin(band * 0.62 / bars, c.px(horizontal ? 52 : 72));
        const qreal gap = bars > 1 ? c.px(3) : 0;
        const qreal group = thickness * bars + gap * (bars - 1);
        double positive = 0, negative = 0;
        for (int si = 0; si < d.series.size(); ++si) {
            const double v = d.series[si].values.value(ci);
            double from = 0, to = v;
            if (stacked) {
                from = v >= 0 ? positive : negative;
                to = from + v;
                (v >= 0 ? positive : negative) = to;
            }
            const qreal offset = stacked ? 0 : si * (thickness + gap);
            const qreal a = toAxis(from), b = toAxis(to);
            QRectF r = horizontal ? QRectF(qMin(a, b), start + (band - group) / 2 + offset, qAbs(b - a), thickness)
                                  : QRectF(start + (band - group) / 2 + offset, qMin(a, b), thickness, qAbs(b - a));
            QColor color = colors[si];
            if (dim) color = mix(color, n.ink.bg, 0.65);
            const bool top = !stacked || si == d.series.size() - 1;
            QPainterPath path = top ? barPath(r, v >= 0) : QPainterPath();
            if (!top) path.addRect(stacked && si > 0 ? (horizontal ? r.adjusted(c.px(1), 0, 0, 0) : r.adjusted(0, 0, 0, -c.px(1))) : r);
            if (stacked && si > 0 && top) path = barPath(horizontal ? r.adjusted(c.px(1), 0, 0, 0) : r.adjusted(0, 0, 0, -c.px(1)), v >= 0);
            p->fillPath(path, color);
            if (showValues && !stacked) {
                const QString text = d.series[si].labels.value(ci);
                if (horizontal)
                    drawLabel(p, QRectF(r.right() + c.px(8), r.top() - 4, vm.horizontalAdvance(text) + 8, r.height() + 8), text,
                              value, ink, Qt::AlignLeft | Qt::AlignVCenter);
                else
                    drawLabel(p, QRectF(r.center().x() - band / 2, r.top() - vm.height() * 1.2, band, vm.height()), text, value,
                              ink, Qt::AlignHCenter | Qt::AlignBottom);
            }
        }
        if (stacked && showValues) {
            double total = 0;
            for (const Series &s : d.series) total += s.values.value(ci);
            const qreal end = toAxis(total);
            if (horizontal)
                drawLabel(p, QRectF(end + c.px(8), start, c.px(160), band), compact(total), value, ink,
                          Qt::AlignLeft | Qt::AlignVCenter);
            else
                drawLabel(p, QRectF(start, end - vm.height() * 1.2, band, vm.height()), compact(total), value, ink,
                          Qt::AlignHCenter | Qt::AlignBottom);
        }
    }
    if (line) {
        for (int si = 0; si < d.series.size(); ++si) {
            QVector<QPointF> points;
            for (int ci = 0; ci < count; ++ci)
                points << QPointF(plot.left() + (ci + 0.5) * band, toAxis(d.series[si].values.value(ci)));
            QPainterPath path(points.value(0));
            const bool smooth = n.flag("smooth", true) && points.size() > 2;
            for (int i = 1; i < points.size(); ++i) {
                if (!smooth) {
                    path.lineTo(points[i]);
                    continue;
                }
                // Catmull-Rom through the points, never overshooting the vertical range of a segment.
                const QPointF p0 = points.value(i - 2, points[i - 1]), p1 = points[i - 1], p2 = points[i],
                              p3 = points.value(i + 1, points[i]);
                QPointF c1 = p1 + (p2 - p0) / 6, c2 = p2 - (p3 - p1) / 6;
                const qreal lo = qMin(p1.y(), p2.y()), hi = qMax(p1.y(), p2.y());
                c1.setY(qBound(lo, c1.y(), hi));
                c2.setY(qBound(lo, c2.y(), hi));
                path.cubicTo(c1, c2, p2);
            }
            if (type == "area") {
                QPainterPath fill = path;
                fill.lineTo(points.last().x(), toAxis(qMax(low, 0.0)));
                fill.lineTo(points.first().x(), toAxis(qMax(low, 0.0)));
                fill.closeSubpath();
                QColor wash = colors[si];
                wash.setAlphaF(0.14);
                p->fillPath(fill, wash);
            }
            p->setPen(QPen(colors[si], c.px(4.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush);
            p->drawPath(path);
            if (inlineLegend && !points.isEmpty()) {
                // Name each line just above its start, keyed by a short stroke in its color.
                const bool rising = points.size() > 1 && points[1].y() < points[0].y() - 1;
                const QPointF at = points.first() + QPointF(c.px(10), rising ? lm.height() * 1.5 : -c.px(14));
                const qreal key = lm.height() * 0.9;
                p->fillRect(QRectF(at.x(), at.y() - lm.height() / 2 - c.px(1.5), key, c.px(3)), colors[si]);
                drawLabel(p, QRectF(at.x() + key + c.px(8), at.y() - lm.height(), plot.width() * 0.6, lm.height()),
                          d.series[si].name, value, ink, Qt::AlignLeft | Qt::AlignVCenter);
            }
            const bool everyPoint = labels == "all";
            for (int i = 0; i < points.size(); ++i) {
                const bool end = i == points.size() - 1;
                if (!end && !everyPoint && points.size() > 8) continue;
                p->setPen(QPen(n.ink.bg, c.px(3)));
                p->setBrush(colors[si]);
                p->drawEllipse(points[i], c.px(end ? 9 : 7), c.px(end ? 9 : 7));
                if ((end && labels != "none") || everyPoint) {
                    const QString text = d.series[si].labels.value(i);
                    if (end && !everyPoint)
                        drawLabel(p, QRectF(points[i].x() + c.px(16), points[i].y() - vm.height() / 2,
                                            area.right() - points[i].x() - c.px(16), vm.height()),
                                  text, value, ink, Qt::AlignLeft | Qt::AlignVCenter);
                    else
                        drawLabel(p, QRectF(points[i].x() - band / 2, points[i].y() - vm.height() * 1.5, band, vm.height()),
                                  text, value, ink, Qt::AlignHCenter | Qt::AlignBottom);
                }
            }
        }
    }
    for (const Mark &mark : marks) {
        if (horizontal) break;
        const qreal x = plot.left() + (mark.category + 0.5) * band;
        p->setPen(QPen(n.ink.accent, c.px(2.5)));
        p->drawLine(QPointF(x, plot.top() - vm.height() * 0.3), QPointF(x, plot.bottom()));
        if (!mark.label.isEmpty()) {
            auto doc = words(mark.label, 17 * k, n.ink.accent, n.ink.accent, Qt::AlignHCenter, c, false, 700, 1.1);
            const qreal width = qMin(band * 1.4, c.px(260));
            const qreal h = heightAt(*doc, width);
            drawText(*doc, QPointF(x - width / 2, plot.top() - vm.height() * 0.4 - h), p);
        }
    }
    if (ok && !std::isnan(goal)) {
        const qreal at = toAxis(goal);
        // A reference threshold, not a gridline: dashed so it reads as a target.
        p->setPen(QPen(ink, c.px(2), Qt::DashLine));
        if (horizontal) p->drawLine(QPointF(at, plot.top()), QPointF(at, plot.bottom()));
        else p->drawLine(QPointF(plot.left(), at), QPointF(plot.right(), at));
        const QString text = n.prop("goal-label", "Goal " + n.prop("goal"));
        const qreal w = vm.horizontalAdvance(text) + c.px(16);
        // Put the label at whichever end of the line the data leaves clear.
        double first = 0, last = 0;
        for (const Series &s : d.series) {
            first = qMax(first, s.values.value(0));
            last = qMax(last, s.values.value(count - 1));
        }
        const bool left = std::abs(first - goal) > std::abs(last - goal);
        if (horizontal)
            drawLabel(p, QRectF(at - w / 2, plot.top() - vm.height() * 1.2, w, vm.height()), text, value, ink);
        else if (left)
            drawLabel(p, QRectF(plot.left() + c.px(8), at - vm.height() * 1.25, w, vm.height()), text, value, ink,
                      Qt::AlignLeft | Qt::AlignBottom);
        else
            drawLabel(p, QRectF(plot.right() - w, at - vm.height() * 1.25, w, vm.height()), text, value, ink,
                      Qt::AlignRight | Qt::AlignBottom);
    }
}

// ---------------------------------------------------------------------------
// Progress

struct Bar {
    QString label, shown;
    double value = 0, max = 100;
};
QVector<Bar> progressBars(const Node &n) {
    QVector<Bar> bars;
    const Table table = parseTable(n.text);
    if (!table.rows.isEmpty()) {
        for (const QStringList &row : table.rows) {
            Bar b;
            b.label = plainText(row.value(0));
            b.value = parseNumber(row.value(1));
            b.max = row.size() > 2 && !row[2].isEmpty() ? parseNumber(row[2]) : parseNumber(n.prop("max", "100"));
            b.shown = plainText(row.value(1));
            bars << b;
        }
    } else {
        Bar b;
        b.label = n.prop("label", plainText(n.text));
        b.value = parseNumber(n.prop("value", "0"));
        b.max = parseNumber(n.prop("max", "100"));
        b.shown = n.prop("value");
        bars << b;
    }
    return bars;
}
qreal progressRow(const Node &n, Ctx &c) {
    const qreal k = scaleOf(n);
    return QFontMetricsF(uiFont(c, c.px(22 * k))).height() * 1.35 + c.px(n.prop("thickness", "16").toDouble() * k);
}

void paintProgress(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const qreal k = scaleOf(n);
    const QFont label = uiFont(c, c.px(22 * k), false, 600), value = uiFont(c, c.px(22 * k), false, 700);
    const qreal thickness = c.px(n.prop("thickness", "16").toDouble() * k);
    const qreal row = progressRow(n, c), gap = c.px(22 * k);
    const QVector<Bar> bars = progressBars(n);
    const QColor tone = toneOr(n, c);
    qreal y = n.rect.top();
    for (const Bar &b : bars) {
        const double t = b.max > 0 ? qBound(0.0, b.value / b.max, 1.0) : 0;
        const QString show = n.prop("show", "value");
        const QString shown = show == "none" ? QString() : show == "percent" ? QString::number(qRound(t * 100)) + "%" : b.shown;
        const qreal textH = row - thickness;
        const qreal valueW = QFontMetricsF(value).horizontalAdvance(shown) + c.px(8);
        drawLabel(p, QRectF(n.rect.left(), y, n.rect.width() - valueW, textH * 0.9), b.label, label, n.ink.fg,
                  Qt::AlignLeft | Qt::AlignVCenter);
        drawLabel(p, QRectF(n.rect.right() - valueW, y, valueW, textH * 0.9), shown, value, n.ink.fg,
                  Qt::AlignRight | Qt::AlignVCenter);
        const QRectF track(n.rect.left(), y + textH, n.rect.width(), thickness);
        p->fillPath(roundedPath(track, thickness / 2, thickness / 2, thickness / 2, thickness / 2), mix(tone, n.ink.bg, 0.8));
        if (t > 0) {
            const QRectF fill(track.left(), track.top(), qMax(thickness, track.width() * t), thickness);
            paintFill(p, roundedPath(fill, thickness / 2, thickness / 2, thickness / 2, thickness / 2),
                      parseFill(n.prop("tone", "accent"), n.ink, c), fill);
        }
        y += row + gap;
    }
}

// ---------------------------------------------------------------------------
// Gantt

struct Task {
    QString name, group, tone;
    double start = 0, end = 0;
    bool milestone = false;
};
struct Schedule {
    QVector<Task> tasks;
    double start = 0, end = 0;
    bool dates = false;
    QString error;
};

int monthOf(const QString &word) {
    static const QStringList months{"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    return months.indexOf(word.left(3).toLower()) + 1;
}

// Days since 2000 for dates, or plain units. `end` rounds month-level dates up.
double timeValue(QString text, bool end, int &year, bool *ok, bool *date) {
    text = plainText(text).trimmed();
    *ok = true;
    QDate d = QDate::fromString(text, Qt::ISODate);
    static const QRegularExpression ym("^(\\d{4})-(\\d{1,2})$"), my("^([A-Za-z]{3,9})\\.?\\s*(\\d{4})?$"),
        q("^Q([1-4])\\s*(\\d{4})?$", QRegularExpression::CaseInsensitiveOption),
        week("^(?:W|Week\\s*)(\\d+)$", QRegularExpression::CaseInsensitiveOption);
    const QDate base(2000, 1, 1);
    if (d.isValid()) {
        *date = true;
        year = d.year();
        return base.daysTo(d) + (end ? 1 : 0);
    }
    if (auto m = ym.match(text); m.hasMatch()) {
        *date = true;
        d = QDate(m.captured(1).toInt(), m.captured(2).toInt(), 1);
        year = d.year();
        return base.daysTo(end ? d.addMonths(1) : d);
    }
    if (auto m = my.match(text); m.hasMatch() && monthOf(m.captured(1)) > 0) {
        *date = true;
        if (!m.captured(2).isEmpty()) year = m.captured(2).toInt();
        d = QDate(year, monthOf(m.captured(1)), 1);
        return base.daysTo(end ? d.addMonths(1) : d);
    }
    if (auto m = q.match(text); m.hasMatch()) {
        *date = true;
        if (!m.captured(2).isEmpty()) year = m.captured(2).toInt();
        d = QDate(year, (m.captured(1).toInt() - 1) * 3 + 1, 1);
        return base.daysTo(end ? d.addMonths(3) : d);
    }
    if (auto m = week.match(text); m.hasMatch()) return (m.captured(1).toDouble() - (end ? 0 : 1)) * 7;
    bool number = false;
    const double v = text.toDouble(&number);
    if (number) return v - (end ? 0 : 1);
    *ok = false;
    return 0;
}

Schedule schedule(const Node &n) {
    Schedule s;
    const Table table = parseTable(n.text);
    if (table.header.size() < 2 || table.rows.isEmpty()) {
        s.error = "needs a Markdown table with Task, Start and End columns (and optionally Group and Tone)";
        return s;
    }
    auto column = [&](const QStringList &names, int fallback) {
        for (int i = 0; i < table.header.size(); ++i)
            if (names.contains(plainText(table.header[i]).toLower())) return i;
        return fallback;
    };
    const int name = column({"task", "name", "item", "activity", "phase name"}, 0);
    const int start = column({"start", "from", "begin", "starts"}, 1);
    const int end = column({"end", "to", "finish", "due", "ends"}, 2);
    const int group = column({"group", "phase", "lane", "team", "stream"}, -1);
    const int tone = column({"tone", "color", "colour"}, -1);
    int year = QDate::currentDate().year();
    if (n.has("start")) {
        bool ok, date;
        timeValue(n.prop("start"), false, year, &ok, &date);
    }
    double previous = -1e9;
    bool first = true;
    for (const QStringList &row : table.rows) {
        Task t;
        t.name = plainText(row.value(name));
        if (group >= 0) t.group = plainText(row.value(group));
        if (tone >= 0) t.tone = plainText(row.value(tone));
        bool ok = false, date = false;
        const int before = year;
        t.start = timeValue(row.value(start), false, year, &ok, &date);
        if (!ok) {
            s.error = "can't read the start “" + row.value(start) + "” as a date, month, quarter or week";
            return s;
        }
        Q_UNUSED(before);
        Q_UNUSED(previous);
        s.dates = s.dates || date;
        const QString endText = row.value(end).trimmed();
        if (endText.isEmpty() || endText == "-" || plainText(endText) == plainText(row.value(start))) {
            t.end = t.start;
            t.milestone = endText.isEmpty() || endText == "-";
            if (!t.milestone) t.end = timeValue(endText, true, year, &ok, &date);
        } else {
            int endYear = year;
            t.end = timeValue(endText, true, endYear, &ok, &date);
            if (ok && t.end <= t.start && !QRegularExpression("\\d{4}").match(endText).hasMatch()) {
                ++endYear;
                t.end = timeValue(endText, true, endYear, &ok, &date);
            }
            if (!ok) {
                s.error = "can't read the end “" + endText + "” as a date, month, quarter or week";
                return s;
            }
        }
        s.start = first ? t.start : qMin(s.start, t.start);
        s.end = first ? t.end : qMax(s.end, t.end);
        first = false;
        s.tasks << t;
    }
    int y = year;
    bool ok, date;
    if (n.has("start")) s.start = timeValue(n.prop("start"), false, y, &ok, &date);
    if (n.has("end")) s.end = timeValue(n.prop("end"), true, y, &ok, &date);
    if (s.end <= s.start) s.end = s.start + 1;
    return s;
}

qreal ganttRow(const Node &n, Ctx &c) { return c.px(50 * scaleOf(n)); }
QStringList ganttGroups(const Schedule &s) {
    QStringList groups;
    for (const Task &t : s.tasks)
        if (!t.group.isEmpty() && !groups.contains(t.group)) groups << t.group;
    return groups;
}
QFont ganttGroupFont(const Node &n, Ctx &c) { return uiFont(c, c.px(17 * scaleOf(n)), false, 800); }
// Room after each group so a turned label fits beside even a one-row group.
QVector<qreal> ganttSideRoom(const Node &n, const Schedule &s, Ctx &c) {
    const QStringList groups = ganttGroups(s);
    QVector<qreal> room(groups.size());
    const QFontMetricsF metrics(ganttGroupFont(n, c));
    for (int g = 0; g < groups.size(); ++g) {
        int rows = 0;
        for (const Task &t : s.tasks) rows += t.group == groups[g];
        room[g] = qMax(0.0, metrics.horizontalAdvance(groups[g].toUpper()) + c.px(28) - rows * ganttRow(n, c));
    }
    return room;
}
qreal measureGantt(Node &n, qreal, Ctx &c) {
    const Schedule s = schedule(n);
    const int groups = ganttGroups(s).size();
    // Groups take a header row above their tasks, or a gap and a turned label beside them.
    if (n.prop("groups") == "side") {
        qreal room = 0;
        for (qreal r : ganttSideRoom(n, s, c)) room += r;
        return c.px(56 * scaleOf(n)) + (s.tasks.size() + qMax(0, groups - 1) * 0.35) * ganttRow(n, c) + room;
    }
    return c.px(56 * scaleOf(n)) + (s.tasks.size() + groups * 0.75) * ganttRow(n, c);
}

void paintGantt(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const Schedule s = schedule(n);
    if (!s.error.isEmpty()) return;
    const qreal k = scaleOf(n);
    const QFont label = uiFont(c, c.px(20 * k)), bold = uiFont(c, c.px(19 * k), false, 700),
                group = ganttGroupFont(n, c);
    const QFontMetricsF lm(label);
    const bool inside = n.prop("labels") == "inside";
    const QStringList groups = ganttGroups(s);
    const bool side = n.prop("groups") == "side" && !groups.isEmpty();
    const qreal sideWidth = side ? c.px(70 * k) : 0;
    qreal nameWidth = 0;
    for (const Task &t : s.tasks) nameWidth = qMax(nameWidth, lm.horizontalAdvance(t.name));
    nameWidth = inside ? 0 : qMin(nameWidth + c.px(28), n.rect.width() * 0.3);
    const qreal left = n.rect.left() + sideWidth;
    const QRectF axis(left + nameWidth, n.rect.top(), n.rect.right() - left - nameWidth, c.px(56 * k));
    const qreal row = ganttRow(n, c);
    const QRectF plot(axis.left(), axis.bottom(), axis.width(), n.rect.bottom() - axis.bottom());
    auto x = [&](double v) { return plot.left() + (v - s.start) / (s.end - s.start) * plot.width(); };
    const QColor grid = mix(n.ink.bg, n.ink.fg, 0.12), muted = mix(n.ink.fg, n.ink.bg, 0.35);
    // Time axis: months for dates, otherwise whole units.
    p->setFont(bold);
    const QDate base(2000, 1, 1);
    if (s.dates) {
        const double span = s.end - s.start;
        const int every = span > 900 ? 3 : 1;
        QDate d = base.addDays(qint64(s.start));
        d = QDate(d.year(), d.month(), 1);
        bool firstLabel = true;
        for (; base.daysTo(d) <= s.end; d = d.addMonths(every)) {
            const double v = base.daysTo(d);
            const QDate next = d.addMonths(every);
            const qreal left = x(qMax(v, s.start)), right = x(qMin(double(base.daysTo(next)), s.end));
            if (v >= s.start) {
                p->setPen(QPen(grid, c.px(1.2)));
                p->drawLine(QPointF(x(v), axis.bottom() - c.px(6)), QPointF(x(v), plot.bottom()));
            }
            QString text = QLocale(QLocale::English).monthName(d.month(), QLocale::ShortFormat);
            if (every == 3) text = "Q" + QString::number((d.month() - 1) / 3 + 1);
            if (firstLabel || d.month() <= every) text += " " + QString::number(d.year()).right(2).prepend('\'');
            firstLabel = false;
            if (right - left > lm.horizontalAdvance("Mmm") * 0.8)
                drawLabel(p, QRectF(left, axis.top(), right - left, axis.height() * 0.8), text, bold, n.ink.fg);
        }
    } else {
        const double step = niceStep(s.end - s.start, 8);
        for (double v = std::ceil(s.start / step) * step; v <= s.end; v += step) {
            p->setPen(QPen(grid, c.px(1.2)));
            p->drawLine(QPointF(x(v), axis.bottom() - c.px(6)), QPointF(x(v), plot.bottom()));
            if (v < s.end)
                drawLabel(p, QRectF(x(v), axis.top(), x(qMin(v + step, s.end)) - x(v), axis.height() * 0.8),
                          QString::number(v + 1), bold, n.ink.fg);
        }
    }
    qreal y = plot.top();
    QString current;
    struct Span {
        int group;
        qreal top, bottom;
    };
    QVector<Span> spans;
    for (int i = 0; i < s.tasks.size(); ++i) {
        const Task &t = s.tasks[i];
        const int groupIndex = groups.indexOf(t.group);
        if (!t.group.isEmpty() && t.group != current) {
            current = t.group;
            if (side) {
                if (!spans.isEmpty()) y += row * 0.35 + ganttSideRoom(n, s, c).value(spans.last().group);
                spans.append({groupIndex, y, y});
            } else {
                drawLabel(p, QRectF(n.rect.left(), y, n.rect.width(), row * 0.75), t.group.toUpper(), group,
                          seriesColor(groupIndex, n.ink, c, colorList(n)), Qt::AlignLeft | Qt::AlignBottom);
                y += row * 0.75;
            }
        }
        if (side && !spans.isEmpty()) spans.last().bottom = y + row;
        QColor color = !t.tone.isEmpty() && c.tone(t.tone, n.ink).isValid() ? c.tone(t.tone, n.ink)
                       : groupIndex >= 0 ? seriesColor(groupIndex, n.ink, c, colorList(n))
                                         : toneOr(n, c);
        if (!inside)
            drawLabel(p, QRectF(left, y, nameWidth - c.px(20), row), t.name, label, n.ink.fg,
                      Qt::AlignLeft | Qt::AlignVCenter);
        if (t.milestone) {
            const qreal r = row * 0.28;
            const QPointF center(x(t.start), y + row / 2);
            QPainterPath diamond;
            diamond.moveTo(center + QPointF(0, -r));
            diamond.lineTo(center + QPointF(r, 0));
            diamond.lineTo(center + QPointF(0, r));
            diamond.lineTo(center + QPointF(-r, 0));
            diamond.closeSubpath();
            p->fillPath(diamond, color);
            if (inside)
                drawLabel(p, QRectF(center.x() + r + c.px(10), y, n.rect.right() - center.x() - r, row), t.name, bold,
                          n.ink.fg, Qt::AlignLeft | Qt::AlignVCenter);
        } else {
            const qreal h = row * 0.56;
            const QRectF bar(x(t.start), y + (row - h) / 2, qMax(h, x(t.end) - x(t.start)), h);
            p->fillPath(roundedPath(bar, h / 2.6, h / 2.6, h / 2.6, h / 2.6), color);
            if (inside) {
                // Inside the bar when the name fits, otherwise just after it.
                const QFont small = uiFont(c, c.px(17 * k), false, 700);
                const qreal need = QFontMetricsF(small).horizontalAdvance(t.name) + c.px(28);
                const qreal after = n.rect.right() - bar.right() - c.px(10), before = bar.left() - plot.left() - c.px(10);
                const qreal wanted = QFontMetricsF(label).horizontalAdvance(t.name);
                if (need <= bar.width())
                    drawLabel(p, bar, t.name, small, inkOver(color, n.ink, c).fg);
                else if (after >= wanted || after >= before)
                    drawLabel(p, QRectF(bar.right() + c.px(10), bar.top() - c.px(4), after, bar.height() + c.px(8)),
                              t.name, label, n.ink.fg, Qt::AlignLeft | Qt::AlignVCenter);
                else
                    drawLabel(p, QRectF(plot.left(), bar.top() - c.px(4), before, bar.height() + c.px(8)), t.name, label,
                              n.ink.fg, Qt::AlignRight | Qt::AlignVCenter);
            }
        }
        y += row;
    }
    const QVector<qreal> room = side ? ganttSideRoom(n, s, c) : QVector<qreal>();
    for (const Span &span : spans) {
        // A bracket beside the group's rows, labelled along its length.
        const QColor color = seriesColor(span.group, n.ink, c, colorList(n));
        const QRectF box(n.rect.left(), span.top + c.px(4), sideWidth * 0.62,
                         span.bottom - span.top + room.value(span.group) - c.px(8));
        p->setPen(QPen(color, c.px(2.5)));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(box, c.px(6), c.px(6));
        p->save();
        p->translate(box.center());
        p->rotate(-90);
        drawLabel(p, QRectF(-box.height() / 2 + c.px(6), -box.width() / 2, box.height() - c.px(12), box.width()),
                  groups[span.group].toUpper(), group, color);
        p->restore();
    }
    if (n.has("today")) {
        int year = QDate::currentDate().year();
        bool ok = false, date = false;
        const double today = timeValue(n.prop("today"), false, year, &ok, &date);
        if (ok && today >= s.start && today <= s.end) {
            p->setPen(QPen(n.ink.accent, c.px(3)));
            p->drawLine(QPointF(x(today), axis.bottom() - c.px(4)), QPointF(x(today), plot.bottom()));
            drawLabel(p, QRectF(x(today) - c.px(60), axis.bottom() - lm.height() * 1.1, c.px(120), lm.height()), "Today",
                      bold, n.ink.accent);
        }
    }
    Q_UNUSED(muted);
}

// ---------------------------------------------------------------------------
// Timeline, Steps, Cycle, Funnel and Venn share list items.

qreal measureTimeline(Node &n, qreal w, Ctx &c) {
    const QVector<Item> items = parseItems(n.text);
    const qreal k = scaleOf(n);
    if (items.isEmpty()) return 0;
    if (n.prop("direction") == "vertical") {
        qreal h = 0;
        for (const Item &item : items) {
            Item copy = item;
            const ItemText t = itemText(copy, n, c, Qt::AlignLeft, 26 * k, 21 * k);
            h += qMax(c.px(44 * k), t.height(w - c.px(76 * k), c.px(4))) + c.px(28 * k);
        }
        return h - c.px(28 * k);
    }
    const qreal column = w / items.size();
    qreal tallest = 0;
    for (const Item &item : items) {
        Item copy = item;
        copy.title.clear();
        const ItemText t = itemText(copy, n, c, Qt::AlignLeft, 24 * k, 20 * k);
        tallest = qMax(tallest, t.height(column - c.px(24), c.px(6)));
    }
    return c.px(40 * k) + c.px(52 * k) + c.px(18 * k) + tallest;
}

QStringList splitHead(const QString &text) {
    const int br = text.indexOf("  \n");
    if (br < 0) return {text, QString()};
    return {text.left(br), text.mid(br + 3)};
}

void paintTimeline(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QVector<Item> items = parseItems(n.text);
    if (items.isEmpty()) return;
    const qreal k = scaleOf(n);
    const QColor tone = toneOr(n, c), line = mix(n.ink.bg, n.ink.fg, 0.2);
    const QString style = n.prop("style", "dot");
    const QFont number = uiFont(c, c.px(20 * k), false, 800);
    auto marker = [&](QPointF center, int i, const Item &item, int count) {
        const QColor color = n.has("colors") ? seriesColor(i, n.ink, c, colorList(n)) : tone;
        Q_UNUSED(count);
        if (style == "number" || (style == "icon" && !item.icon.isEmpty())) {
            const qreal r = c.px(24 * k);
            p->setPen(Qt::NoPen);
            p->setBrush(color);
            p->drawEllipse(center, r, r);
            const QColor on = inkOver(color, n.ink, c).fg;
            if (style == "icon")
                paintIcon(p, item.icon, QRectF(center - QPointF(r * 0.6, r * 0.6), QSizeF(r * 1.2, r * 1.2)), on, 2);
            else
                drawLabel(p, QRectF(center - QPointF(r, r), QSizeF(2 * r, 2 * r)), QString::number(i + 1), number, on);
        } else {
            p->setPen(QPen(n.ink.bg, c.px(5)));
            p->setBrush(color);
            p->drawEllipse(center, c.px(13 * k), c.px(13 * k));
        }
    };
    if (n.prop("direction") == "vertical") {
        const qreal x = n.rect.left() + c.px(24 * k);
        qreal y = n.rect.top();
        QVector<qreal> centers;
        QVector<ItemText> texts;
        QVector<qreal> heights;
        for (const Item &item : items) {
            ItemText t = itemText(item, n, c, Qt::AlignLeft, 26 * k, 21 * k);
            const qreal h = qMax(c.px(44 * k), t.height(n.rect.width() - c.px(76 * k), c.px(4)));
            centers << y + c.px(22 * k);
            texts << t;
            heights << h;
            y += h + c.px(28 * k);
        }
        p->setPen(QPen(line, c.px(3)));
        p->drawLine(QPointF(x, centers.first()), QPointF(x, centers.last()));
        for (int i = 0; i < items.size(); ++i) {
            marker(QPointF(x, centers[i]), i, items[i], items.size());
            texts[i].paint(p, QPointF(n.rect.left() + c.px(76 * k), centers[i] - c.px(18 * k)),
                           n.rect.width() - c.px(76 * k), c.px(4));
        }
        return;
    }
    const qreal column = n.rect.width() / items.size();
    const qreal labelH = c.px(40 * k), axisY = n.rect.top() + labelH + c.px(26 * k);
    p->setPen(QPen(line, c.px(3)));
    p->drawLine(QPointF(n.rect.left() + column / 2, axisY), QPointF(n.rect.right() - column / 2, axisY));
    const QFont title = uiFont(c, c.px(26 * k), true);
    for (int i = 0; i < items.size(); ++i) {
        const qreal cx = n.rect.left() + column * (i + 0.5);
        const QColor color = n.has("colors") ? seriesColor(i, n.ink, c, colorList(n)) : tone;
        drawLabel(p, QRectF(cx - column / 2, n.rect.top(), column, labelH), items[i].title, title,
                  contrast(color, n.ink.bg) >= 3 ? color : n.ink.fg);
        marker(QPointF(cx, axisY), i, items[i], items.size());
        Item copy = items[i];
        copy.title.clear();
        const QStringList head = splitHead(copy.text);
        ItemText t;
        t.title = head[0].isEmpty() ? nullptr : words(head[0], 24 * k, n.ink.fg, n.ink.accent, Qt::AlignHCenter, c, true);
        if (!head[1].isEmpty())
            t.body = words(head[1], 20 * k, mix(n.ink.fg, n.ink.bg, 0.2), n.ink.accent, Qt::AlignHCenter, c);
        t.paint(p, QPointF(cx - column / 2 + c.px(12), axisY + c.px(42 * k)), column - c.px(24), c.px(6));
    }
}

qreal hubHeight(const Node &n, Ctx &c) {
    return n.has("hub") && n.prop("direction") != "vertical" ? c.px(150 * scaleOf(n)) : 0;
}
qreal measureStepsBody(Node &n, qreal w, Ctx &c);
qreal measureSteps(Node &n, qreal w, Ctx &c) { return measureStepsBody(n, w, c) + hubHeight(n, c); }
qreal measureStepsBody(Node &n, qreal w, Ctx &c) {
    const QVector<Item> items = parseItems(n.text);
    if (items.isEmpty()) return 0;
    const qreal k = scaleOf(n);
    const QString style = n.prop("style", "chevron");
    if (n.prop("direction") == "vertical") {
        qreal h = 0;
        for (const Item &item : items) {
            const ItemText t = itemText(item, n, c, Qt::AlignLeft, 26 * k, 21 * k);
            h += qMax(c.px(56 * k), t.height(w - c.px(90 * k), c.px(4))) + c.px(26 * k);
        }
        return h - c.px(26 * k);
    }
    const qreal gap = style == "chevron" ? c.px(6) : c.px(44 * k);
    const qreal column = (w - gap * (items.size() - 1)) / items.size();
    qreal tallest = 0;
    for (const Item &item : items) {
        Item copy = item;
        if (style == "chevron") copy.title.clear();
        const ItemText t = itemText(copy, n, c, Qt::AlignLeft, 26 * k, 20 * k);
        tallest = qMax(tallest, t.height(column - c.px(style == "card" || style == "arrow" ? 56 : 16), c.px(8)));
    }
    if (style == "chevron") return c.px(96 * k) + (tallest > 0 ? c.px(20) + tallest : 0);
    if (style == "circle") return c.px(72 * k) + c.px(22 * k) + tallest;
    const bool icons = std::any_of(items.begin(), items.end(), [](const Item &i) { return !i.icon.isEmpty(); });
    return tallest + c.px(56) + (icons || n.flag("numbered", true) ? c.px(70 * k) : 0);
}

void paintStepsBody(Node &n, Ctx &c);
void paintSteps(Node &n, Ctx &c) {
    const qreal hub = hubHeight(n, c);
    if (hub <= 0) {
        paintStepsBody(n, c);
        return;
    }
    // Every step feeds one shared practice: stems join them to a bus and a labelled pill.
    const QRectF full = n.rect;
    n.rect.setHeight(full.height() - hub);
    paintStepsBody(n, c);
    n.rect = full;
    QPainter *p = c.p;
    const int count = parseItems(n.text).size();
    const qreal k = scaleOf(n);
    const qreal gap = n.prop("style", "chevron") == "chevron" ? c.px(6) : c.px(44 * k);
    const qreal column = (full.width() - gap * (count - 1)) / count;
    const qreal top = full.bottom() - hub, bus = top + hub * 0.3;
    const QColor line = mix(n.ink.bg, n.ink.fg, 0.35);
    p->setPen(QPen(line, c.px(2.5)));
    for (int i = 0; i < count; ++i) {
        const qreal x = full.left() + i * (column + gap) + column / 2;
        p->drawLine(QPointF(x, top + c.px(8)), QPointF(x, bus));
    }
    p->drawLine(QPointF(full.left() + column / 2, bus), QPointF(full.right() - column / 2, bus));
    const QFont font = uiFont(c, c.type(24 * k), false, 700);
    const qreal height = QFontMetricsF(font).height() + c.px(24 * k);
    const qreal width = qMin(full.width() * 0.6, QFontMetricsF(font).horizontalAdvance(n.prop("hub")) + c.px(64 * k));
    const QRectF pill(full.center().x() - width / 2, full.bottom() - height - c.px(4), width, height);
    p->drawLine(QPointF(full.center().x(), bus), QPointF(full.center().x(), pill.top()));
    const Fill fill = parseFill(n.prop("hub-tone", "gradient"), n.ink, c);
    paintFill(p, roundedPath(pill, height / 2, height / 2, height / 2, height / 2), fill, pill);
    drawLabel(p, pill.adjusted(c.px(20), 0, -c.px(20), 0), n.prop("hub"), font, inkOver(fill.average(), n.ink, c).fg);
}
void paintStepsBody(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QVector<Item> items = parseItems(n.text);
    if (items.isEmpty()) return;
    const qreal k = scaleOf(n);
    const QString style = n.prop("style", "chevron");
    const QColor tone = toneOr(n, c);
    const bool numbered = n.flag("numbered", true);
    auto colorAt = [&](int i) {
        if (n.has("colors")) return seriesColor(i, n.ink, c, colorList(n));
        if (style == "chevron" && items.size() > 1) {
            // One hue stepping darker: an ordered process, not categories.
            return mix(tone, n.ink.dark ? QColor(Qt::white) : QColor(Qt::black), 0.32 * i / (items.size() - 1));
        }
        return tone;
    };
    const QFont number = uiFont(c, c.px(24 * k), true);
    if (n.prop("direction") == "vertical") {
        qreal y = n.rect.top();
        const qreal r = c.px(28 * k), x = n.rect.left() + r;
        QVector<qreal> centers;
        for (int i = 0; i < items.size(); ++i) {
            const ItemText t = itemText(items[i], n, c, Qt::AlignLeft, 26 * k, 21 * k);
            const qreal h = qMax(c.px(56 * k), t.height(n.rect.width() - c.px(90 * k), c.px(4)));
            centers << y + r;
            t.paint(p, QPointF(n.rect.left() + c.px(90 * k), y + c.px(8 * k)), n.rect.width() - c.px(90 * k), c.px(4));
            y += h + c.px(26 * k);
        }
        p->setPen(QPen(mix(n.ink.bg, n.ink.fg, 0.2), c.px(3)));
        p->drawLine(QPointF(x, centers.first()), QPointF(x, centers.last()));
        for (int i = 0; i < items.size(); ++i) {
            const QColor color = colorAt(i);
            p->setPen(QPen(n.ink.bg, c.px(4)));
            p->setBrush(color);
            p->drawEllipse(QPointF(x, centers[i]), r, r);
            const QColor on = inkOver(color, n.ink, c).fg;
            if (!items[i].icon.isEmpty())
                paintIcon(p, items[i].icon, QRectF(QPointF(x, centers[i]) - QPointF(r * 0.55, r * 0.55), QSizeF(r * 1.1, r * 1.1)), on, 2);
            else
                drawLabel(p, QRectF(x - r, centers[i] - r, 2 * r, 2 * r), QString::number(i + 1), number, on);
        }
        return;
    }
    const qreal gap = style == "chevron" ? c.px(6) : c.px(44 * k);
    const qreal column = (n.rect.width() - gap * (items.size() - 1)) / items.size();
    for (int i = 0; i < items.size(); ++i) {
        const qreal left = n.rect.left() + i * (column + gap);
        const QColor color = colorAt(i);
        const QColor on = inkOver(color, n.ink, c).fg;
        if (style == "chevron") {
            const qreal h = c.px(96 * k), tip = h * 0.32;
            const QRectF r(left, n.rect.top(), column, h);
            QPainterPath shape;
            shape.moveTo(r.topLeft());
            shape.lineTo(r.right() - (i + 1 < items.size() ? tip : 0), r.top());
            if (i + 1 < items.size()) shape.lineTo(r.right() + tip * 0.0 + (gap > 0 ? 0 : 0), r.center().y());
            shape.lineTo(r.right() - (i + 1 < items.size() ? tip : 0), r.bottom());
            shape.lineTo(r.left(), r.bottom());
            if (i > 0) shape.lineTo(r.left() + tip, r.center().y());
            shape.closeSubpath();
            p->fillPath(shape, color);
            QRectF labelRect = r.adjusted(i > 0 ? tip + c.px(10) : c.px(22), 0, -(tip + c.px(8)), 0);
            const QString head = (numbered ? QString::number(i + 1) + "  " : QString()) + items[i].title;
            QFont font = uiFont(c, c.px(24 * k), true);
            if (!items[i].icon.isEmpty()) {
                const qreal s = c.px(34 * k);
                paintIcon(p, items[i].icon, QRectF(labelRect.left(), r.center().y() - s / 2, s, s), on, 2);
                labelRect.setLeft(labelRect.left() + s + c.px(12));
            }
            auto doc = words(head.isEmpty() ? QString::number(i + 1) : head, 24 * k, on, on, Qt::AlignLeft, c, true, -1, 1.1);
            const qreal th = heightAt(*doc, labelRect.width());
            drawText(*doc, QPointF(labelRect.left(), r.center().y() - th / 2), p);
            Q_UNUSED(font);
            if (!items[i].text.isEmpty()) {
                auto body = words(items[i].text, 20 * k, mix(n.ink.fg, n.ink.bg, 0.15), n.ink.accent, Qt::AlignLeft, c);
                heightAt(*body, column - c.px(16));
                drawText(*body, QPointF(left + c.px(8), r.bottom() + c.px(20)), p);
            }
            continue;
        }
        if (style == "circle") {
            const qreal r = c.px(36 * k);
            const QPointF center(left + column / 2, n.rect.top() + r);
            if (i + 1 < items.size()) {
                p->setPen(QPen(mix(n.ink.bg, n.ink.fg, 0.22), c.px(3)));
                p->drawLine(center + QPointF(r + c.px(10), 0), QPointF(left + column + gap + column / 2 - r - c.px(10), center.y()));
            }
            p->setPen(Qt::NoPen);
            p->setBrush(color);
            p->drawEllipse(center, r, r);
            if (!items[i].icon.isEmpty())
                paintIcon(p, items[i].icon, QRectF(center - QPointF(r * 0.55, r * 0.55), QSizeF(r * 1.1, r * 1.1)), on, 2);
            else
                drawLabel(p, QRectF(center - QPointF(r, r), QSizeF(2 * r, 2 * r)), QString::number(i + 1), number, on);
            const ItemText t = itemText(items[i], n, c, Qt::AlignHCenter, 26 * k, 20 * k);
            t.paint(p, QPointF(left + c.px(8), center.y() + r + c.px(22 * k)), column - c.px(16), c.px(8));
            continue;
        }
        // Cards joined by arrows ("arrow") or set side by side ("card").
        const QRectF card(left, n.rect.top(), column, n.rect.height());
        const qreal radius = c.px(c.radius());
        const QPainterPath path = roundedPath(card, radius, radius, radius, radius);
        const QColor surface = c.color("surface");
        if (c.lightSlide) paintShadow(p, card, radius, c);
        p->fillPath(path, surface);
        if (!c.lightSlide) {
            p->setPen(QPen(n.ink.line, c.px(1.5)));
            p->drawPath(path);
        }
        Ink inside = inkOver(surface, n.ink, c);
        qreal y = card.top() + c.px(28);
        const qreal badge = c.px(54 * k);
        if (!items[i].icon.isEmpty() || numbered) {
            const QRectF b(card.left() + c.px(28), y, badge, badge);
            QColor wash = color;
            wash.setAlphaF(0.14);
            p->fillPath(roundedPath(b, badge * 0.28, badge * 0.28, badge * 0.28, badge * 0.28),
                        items[i].icon.isEmpty() ? color : wash);
            if (!items[i].icon.isEmpty())
                paintIcon(p, items[i].icon, b.adjusted(badge * 0.2, badge * 0.2, -badge * 0.2, -badge * 0.2), color, 2);
            else
                drawLabel(p, b, QString::number(i + 1), number, inkOver(color, inside, c).fg);
            y += badge + c.px(16 * k);
        }
        Node copy = n;
        copy.ink = inside;
        const ItemText t = itemText(items[i], copy, c, Qt::AlignLeft, 26 * k, 20 * k);
        t.paint(p, QPointF(card.left() + c.px(28), y), column - c.px(56), c.px(8));
        if (style == "arrow" && i + 1 < items.size()) {
            const qreal a = c.px(14 * k);
            const QPointF tip(card.right() + gap / 2 + a * 0.6, card.center().y());
            QPainterPath arrow;
            arrow.moveTo(tip);
            arrow.lineTo(tip + QPointF(-a * 1.2, -a));
            arrow.lineTo(tip + QPointF(-a * 1.2, a));
            arrow.closeSubpath();
            p->fillPath(arrow, tone);
        }
    }
}

qreal measureCycle(Node &n, qreal w, Ctx &c) {
    return qBound(c.px(380), w * 0.56, c.px(760)) * scaleOf(n);
}

// Text set along a circle, centered on `middle` (radians, clockwise from the right).
void curvedText(QPainter *p, QPointF center, qreal radius, double middle, const QString &text, const QFont &font,
                const QColor &color) {
    const QFontMetricsF metrics(font);
    const qreal length = metrics.horizontalAdvance(text);
    double angle = middle - length / 2 / radius;
    p->save();
    p->setFont(font);
    p->setPen(color);
    for (const QChar ch : text) {
        const qreal advance = metrics.horizontalAdvance(ch);
        const double at = angle + advance / 2 / radius;
        p->save();
        p->translate(center + QPointF(std::cos(at), std::sin(at)) * radius);
        p->rotate(qRadiansToDegrees(at) + 90);
        p->drawText(QPointF(-advance / 2, metrics.ascent() / 2 - metrics.descent() / 2), QString(ch));
        p->restore();
        angle += advance / radius;
    }
    p->restore();
}
// A disc in the middle of a diagram, showing a picture or a few words.
void paintHub(QPainter *p, const Node &n, QPointF center, qreal radius, qreal k, Ctx &c, bool accent) {
    p->setPen(Qt::NoPen);
    p->setBrush(c.color("surface"));
    p->drawEllipse(center, radius, radius);
    if (n.has("center-image")) {
        const qreal side = radius * 1.25;
        QPainterPath clip;
        clip.addEllipse(center, radius, radius);
        drawPicture(p, c.asset(n.prop("center-image")), QRectF(center - QPointF(side / 2, side / 2), QSizeF(side, side)),
                    clip, {}, false, true);
    } else if (n.has("center")) {
        const Ink over = inkOver(c.color("surface"), n.ink, c);
        const QColor ink = accent ? over.accent : over.fg;
        auto doc = words(n.prop("center"), 34 * k, ink, over.accent, Qt::AlignHCenter, c, true, -1, 1.05);
        const qreal h = fitted(*doc, radius * 1.6, c);
        drawText(*doc, QPointF(center.x() - radius * 0.8, center.y() - h / 2), p);
    }
}

void paintCycle(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QVector<Item> items = parseItems(n.text);
    if (items.isEmpty()) return;
    const qreal k = scaleOf(n);
    const QRectF r = n.rect;
    const qreal labelWidth = qMin(r.width() * 0.26, c.px(360 * k));
    const bool wheeled = n.prop("style") == "wheel";
    // Leave room above and below for the labels of the top and bottom stages.
    const qreal labelRoom = c.px(32 * k) + c.px(24 * k) + c.px(64 * k);
    const qreal radius = qMin((r.height() / 2 - labelRoom) / (wheeled ? 1.0 : 1.0),
                              (r.width() - 2 * labelWidth) / 2 - c.px(60 * k));
    if (radius <= 0) return;
    const QPointF center = r.center();
    const QColor tone = toneOr(n, c), ring = mix(n.ink.bg, n.ink.fg, 0.18);
    const int count = items.size();
    const bool wheel = n.prop("style") == "wheel";
    if (wheel) {
        // A donut of colored stages, split by surface-colored gaps, around a center label.
        const qreal inner = radius * 0.5;
        for (int i = 0; i < count; ++i) {
            const double start = 90 - (i - 0.5) * 360.0 / count, span = -360.0 / count;
            QPainterPath segment;
            segment.arcMoveTo(QRectF(center - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius)), start);
            segment.arcTo(QRectF(center - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius)), start, span);
            segment.arcTo(QRectF(center - QPointF(inner, inner), QSizeF(2 * inner, 2 * inner)), start + span, -span);
            segment.closeSubpath();
            p->fillPath(segment, seriesColor(i, n.ink, c, colorList(n)));
            p->setPen(QPen(n.ink.bg, c.px(8)));
            p->setBrush(Qt::NoBrush);
            p->drawPath(segment);
        }
        paintHub(p, n, center, inner * 0.92, k, c, true);
    } else {
        p->setPen(QPen(ring, c.px(4)));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(center, radius, radius);
        // Arrowheads along the ring show the direction of travel.
        for (int i = 0; i < count; ++i) {
            const double a = -M_PI / 2 + (i + 0.5) * 2 * M_PI / count;
            const QPointF at = center + QPointF(std::cos(a), std::sin(a)) * radius;
            const QPointF dir(-std::sin(a), std::cos(a));
            const QPointF normal(std::cos(a), std::sin(a));
            const qreal s = c.px(13 * k);
            QPainterPath arrow;
            arrow.moveTo(at + dir * s);
            arrow.lineTo(at - dir * s + normal * s * 0.85);
            arrow.lineTo(at - dir * s - normal * s * 0.85);
            arrow.closeSubpath();
            p->fillPath(arrow, ring.darker(115));
        }
        const QStringList phases = n.prop("phases").split(QRegularExpression("\\s*[,;]\\s*"), Qt::SkipEmptyParts);
        if (!phases.isEmpty()) {
            // Phase names ride an inner ring, each centered on its share of the circle.
            const QFont font = uiFont(c, c.type(26 * k), true);
            for (int j = 0; j < phases.size(); ++j)
                curvedText(p, center, radius * 0.7, -M_PI / 2 + j * 2 * M_PI / phases.size(), phases[j], font,
                           mix(n.ink.fg, n.ink.bg, 0.15));
        }
        if (n.has("center") || n.has("center-image")) {
            const qreal hub = phases.isEmpty() ? radius * 0.62 : radius * 0.48;
            if (n.has("center-image") || !phases.isEmpty()) paintHub(p, n, center, hub, k, c, false);
            else {
                auto doc = words(n.prop("center"), 34 * k, n.ink.fg, n.ink.accent, Qt::AlignHCenter, c, true, -1, 1.05);
                const qreal h = fitted(*doc, radius * 1.4, c);
                drawText(*doc, QPointF(center.x() - radius * 0.7, center.y() - h / 2), p);
            }
        }
    }
    const QFont number = uiFont(c, c.px(22 * k), true);
    for (int i = 0; i < count; ++i) {
        const double a = -M_PI / 2 + i * 2 * M_PI / count;
        const QPointF at = center + QPointF(std::cos(a), std::sin(a)) * (wheel ? radius * 0.75 : radius);
        QColor color = (n.has("colors") || wheel) ? seriesColor(i, n.ink, c, colorList(n)) : tone;
        const qreal node = c.px((wheel ? 40 : 32) * k);
        p->setPen(wheel ? Qt::NoPen : QPen(n.ink.bg, c.px(5)));
        p->setBrush(wheel ? c.color("surface") : color);
        p->drawEllipse(at, node, node);
        const QColor on = wheel ? color : inkOver(color, n.ink, c).fg;
        if (!items[i].icon.isEmpty())
            paintIcon(p, items[i].icon, QRectF(at - QPointF(node * 0.55, node * 0.55), QSizeF(node * 1.1, node * 1.1)), on, 2);
        else if (n.flag("numbered", true))
            drawLabel(p, QRectF(at - QPointF(node, node), QSizeF(2 * node, 2 * node)), QString::number(i + 1).rightJustified(2, '0'), number, on);
        // Labels sit outside the ring, aligned away from it.
        const double cosA = std::cos(a), sinA = std::sin(a);
        Qt::Alignment align = std::abs(cosA) < 0.25 ? Qt::AlignHCenter : cosA > 0 ? Qt::AlignLeft : Qt::AlignRight;
        const ItemText t = itemText(items[i], n, c, align, 24 * k, 19 * k);
        const qreal h = t.height(labelWidth, c.px(4));
        const qreal offset = wheel ? radius * 0.25 + c.px(24 * k) : node + c.px(18 * k);
        QPointF topLeft;
        if (align == Qt::AlignHCenter)
            topLeft = QPointF(at.x() - labelWidth / 2, sinA < 0 ? at.y() - offset - h : at.y() + offset);
        else if (align == Qt::AlignLeft)
            topLeft = QPointF(at.x() + offset, at.y() - h / 2);
        else
            topLeft = QPointF(at.x() - offset - labelWidth, at.y() - h / 2);
        t.paint(p, topLeft, labelWidth, c.px(4));
    }
}

// Funnel ----------------------------------------------------------------------

QStringList axisLabels(const Node &n) {
    const QString axis = n.prop("axis");
    QStringList parts = axis.split(axis.contains(';') ? QRegularExpression("\\s*;\\s*") : QRegularExpression("\\s*\\|\\s*"),
                                   Qt::SkipEmptyParts);
    while (parts.size() < 2 && !parts.isEmpty()) parts << QString();
    return parts.mid(0, 2);
}
bool funnelInside(const Node &n, const QVector<Item> &items) {
    if (n.prop("labels") == "inside") return true;
    if (n.prop("labels") == "side") return false;
    // Side text only when there is something to put there.
    return std::none_of(items.begin(), items.end(), [](const Item &i) { return !i.text.isEmpty() && !i.title.isEmpty(); });
}

// Stages narrow less when they carry text, and each is as tall as its words need.
qreal funnelTaper(const Node &n, const QVector<Item> &items) {
    const bool both = std::any_of(items.begin(), items.end(), [](const Item &i) { return !i.text.isEmpty() && !i.title.isEmpty(); });
    return funnelInside(n, items) && both ? 0.4 : 0.55;
}
qreal funnelAxisWidth(const Node &n, qreal width, Ctx &c) {
    return axisLabels(n).isEmpty() ? 0 : qMin(c.px(170 * scaleOf(n)), width * 0.22);
}
qreal funnelRow(const Node &n, const QVector<Item> &items, qreal width, Ctx &c) {
    const qreal k = scaleOf(n);
    const bool inside = funnelInside(n, items);
    const qreal shapeWidth = (width - funnelAxisWidth(n, width, c)) * (inside ? 1.0 : 0.5);
    const qreal taper = funnelTaper(n, items);
    qreal row = c.px(92 * k);
    for (int i = 0; i < items.size() && inside; ++i) {
        // The narrow end of the stage bounds its text.
        const qreal textWidth = shapeWidth * (1 - taper * (i + 1) / items.size()) * 0.82;
        const QString head = items[i].title.isEmpty() ? items[i].text : items[i].title;
        auto doc = words(head, 26 * k, n.ink.fg, n.ink.fg, Qt::AlignHCenter, c, true, -1, 1.05);
        qreal h = fitted(*doc, textWidth, c);
        if (!items[i].title.isEmpty() && !items[i].text.isEmpty()) {
            auto body = words(items[i].text, 18 * k, n.ink.fg, n.ink.fg, Qt::AlignHCenter, c, false, -1, 1.2);
            h += c.px(6) + fitted(*body, textWidth, c);
        }
        row = qMax(row, h + c.px(28 * k));
    }
    return row;
}
qreal measureFunnel(Node &n, qreal w, Ctx &c) {
    const QVector<Item> items = parseItems(n.text);
    return items.size() * funnelRow(n, items, w, c) + (items.size() - 1) * c.px(6);
}

void paintFunnel(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QVector<Item> items = parseItems(n.text);
    if (items.isEmpty()) return;
    const qreal k = scaleOf(n);
    const bool inside = funnelInside(n, items);
    QRectF r = n.rect;
    const QStringList axis = axisLabels(n);
    const qreal axisWidth = funnelAxisWidth(n, r.width(), c);
    const qreal least = funnelRow(n, items, r.width(), c);
    r.setLeft(r.left() + axisWidth);
    const qreal shapeWidth = inside ? r.width() : r.width() * 0.5;
    const qreal gap = c.px(6);
    const qreal taper = funnelTaper(n, items);
    const qreal row = qMax(least, qMin((r.height() - gap * (items.size() - 1)) / items.size(), least * 1.6));
    const qreal top0 = r.top() + (r.height() - items.size() * (row + gap) + gap) / 2;
    const QColor tone = toneOr(n, c);
    for (int i = 0; i < items.size(); ++i) {
        const qreal top = top0 + i * (row + gap);
        const qreal t0 = double(i) / items.size(), t1 = double(i + 1) / items.size();
        const qreal w0 = shapeWidth * (1 - taper * t0), w1 = shapeWidth * (1 - taper * t1);
        const qreal cx = r.left() + shapeWidth / 2;
        QPainterPath shape;
        shape.moveTo(cx - w0 / 2, top);
        shape.lineTo(cx + w0 / 2, top);
        shape.lineTo(cx + w1 / 2, top + row);
        shape.lineTo(cx - w1 / 2, top + row);
        shape.closeSubpath();
        const QColor color = n.has("colors") ? seriesColor(i, n.ink, c, colorList(n))
                                             : mix(tone, n.ink.dark ? QColor(Qt::white) : QColor(Qt::black),
                                                   items.size() > 1 ? 0.35 * i / (items.size() - 1) : 0);
        p->fillPath(shape, color);
        const QColor on = inkOver(color, n.ink, c).fg;
        const QString head = items[i].title.isEmpty() ? items[i].text : items[i].title;
        const qreal textWidth = w1 * 0.82;
        // A stage's title shrinks to fit its narrow end rather than breaking a word.
        qreal titleSize = 26 * k;
        auto doc = words(head, titleSize, on, on, Qt::AlignHCenter, c, true, -1, 1.05);
        while (titleSize > 14 && (heightAt(*doc, textWidth), doc->idealWidth() > textWidth + 1)) {
            titleSize -= 1;
            doc = words(head, titleSize, on, on, Qt::AlignHCenter, c, true, -1, 1.05);
        }
        qreal h = fitted(*doc, textWidth, c);
        QSharedPointer<QTextDocument> body;
        if (inside && !items[i].title.isEmpty() && !items[i].text.isEmpty()) {
            body = words(items[i].text, 18 * k, mix(on, color, 0.12), on, Qt::AlignHCenter, c, false, -1, 1.2);
            h += c.px(6) + fitted(*body, textWidth, c);
        }
        if (h > row + 1) c.overflow = true;
        const qreal y = top + (row - h) / 2;
        drawText(*doc, QPointF(cx - textWidth / 2, y), p);
        if (body) drawText(*body, QPointF(cx - textWidth / 2, y + doc->size().height() + c.px(6)), p);
        if (!inside && !items[i].text.isEmpty()) {
            auto side = words(items[i].text, 20 * k, n.ink.fg, n.ink.accent, Qt::AlignLeft, c);
            const qreal left = r.left() + shapeWidth + c.px(40);
            const qreal bh = fitted(*side, r.right() - left, c);
            p->setPen(QPen(mix(n.ink.bg, n.ink.fg, 0.25), c.px(1.5), Qt::DotLine));
            p->drawLine(QPointF(cx + (w0 + w1) / 4 + c.px(10), top + row / 2), QPointF(left - c.px(12), top + row / 2));
            drawText(*side, QPointF(left, top + (row - bh) / 2), p);
        }
    }
    if (!axis.isEmpty()) {
        // A double-headed scale beside the stages, labelled at both ends.
        const qreal x = r.left() - c.px(26 * k);
        const qreal top = top0, bottom = top0 + items.size() * (row + gap) - gap;
        const QColor ink = mix(n.ink.fg, n.ink.bg, 0.3);
        auto label = [&](const QString &text, bool upper) {
            auto doc = words(text, 17 * k, ink, ink, Qt::AlignRight, c, false, -1, 1.15);
            const qreal width = axisWidth - c.px(44 * k);
            const qreal h = fitted(*doc, width, c);
            drawText(*doc, QPointF(x - c.px(16 * k) - width, upper ? top : bottom - h), p);
            return h;
        };
        const qreal a = label(axis.value(0), true), b = label(axis.value(1), false);
        Q_UNUSED(a);
        Q_UNUSED(b);
        p->setPen(QPen(ink, c.px(2)));
        p->drawLine(QPointF(x, top + c.px(6)), QPointF(x, bottom - c.px(6)));
        const qreal s = c.px(8 * k);
        for (const qreal y : {top + c.px(6), bottom - c.px(6)}) {
            const qreal d = y < (top + bottom) / 2 ? 1 : -1;
            QPainterPath head;
            head.moveTo(x, y - d * s * 0.2);
            head.lineTo(x - s * 0.8, y + d * s * 1.2);
            head.lineTo(x + s * 0.8, y + d * s * 1.2);
            head.closeSubpath();
            p->fillPath(head, ink);
        }
    }
}

// Venn ------------------------------------------------------------------------

qreal measureVenn(Node &n, qreal w, Ctx &c) { return qBound(c.px(320), w * 0.6, c.px(620)) * scaleOf(n); }

void paintVenn(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QVector<Item> items = parseItems(n.text);
    const int count = qMin(4, int(items.size()));
    if (count < 2) return;
    const qreal k = scaleOf(n);
    const QRectF r = n.rect;
    const qreal spread = 0.62;
    qreal radius = 0;
    if (count == 2) radius = qMin(r.height() / 2, r.width() / (2 + 2 * spread));
    else if (count == 3) radius = qMin(r.height() / 3.3, r.width() / 3.3);
    else radius = qMin(r.height(), r.width()) / (2 + 2 * spread);
    QPointF center = r.center();
    if (count == 3) center.ry() += radius * 0.12;
    QVector<QPointF> centers;
    for (int i = 0; i < count; ++i) {
        // Two side by side, three in a triangle, four as petals around the middle.
        const double a = count == 2 ? M_PI * i : -M_PI / 2 + i * 2 * M_PI / count;
        centers << center + QPointF(std::cos(a), std::sin(a)) * radius * spread;
    }
    for (int i = 0; i < count; ++i) {
        QColor color = seriesColor(i, n.ink, c, colorList(n));
        color.setAlphaF(0.78);
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawEllipse(centers[i], radius, radius);
    }
    const bool middle = n.has("center") || n.has("center-image");
    const qreal hole = radius * (count == 2 ? 0.34 : count == 3 ? 0.4 : 0.5);
    for (int i = 0; i < count; ++i) {
        const QPointF away = centers[i] - center;
        const qreal length = std::hypot(away.x(), away.y());
        const QPointF labelAt = centers[i] + (length > 0 ? away / length * radius * (count == 4 ? 0.36 : 0.42) : QPointF());
        const QColor on = inkOver(seriesColor(i, n.ink, c, colorList(n)), n.ink, c).fg;
        const qreal width = radius * (count == 4 ? 0.95 : 1.0);
        auto doc = words(items[i].title.isEmpty() ? items[i].text : items[i].title, 26 * k, on, on, Qt::AlignHCenter, c,
                         true, -1, 1.05);
        const qreal h = fitted(*doc, width, c);
        drawText(*doc, QPointF(labelAt.x() - width / 2, labelAt.y() - h / 2), p);
    }
    if (middle) {
        p->setPen(Qt::NoPen);
        p->setBrush(c.color("surface"));
        p->drawEllipse(center, hole, hole);
        if (n.has("center-image")) {
            const QRectF box(center - QPointF(hole * 0.7, hole * 0.7), QSizeF(hole * 1.4, hole * 1.4));
            QPainterPath clip;
            clip.addEllipse(center, hole, hole);
            drawPicture(p, c.asset(n.prop("center-image")), box, clip, {}, false, true);
        } else {
            const Ink over = inkOver(c.color("surface"), n.ink, c);
            auto doc = words(n.prop("center"), 20 * k, over.fg, over.accent, Qt::AlignHCenter, c, true, -1, 1.0);
            const qreal h = fitted(*doc, hole * 1.6, c);
            drawText(*doc, QPointF(center.x() - hole * 0.8, center.y() - h / 2), p);
        }
    }
}

// Tree ------------------------------------------------------------------------

struct Branch {
    Item item;
    QVector<int> children;
    int depth = 0;
    double slot = 0; // Center, in leaf units.
};
QVector<Branch> branches(const Node &n) {
    QVector<Branch> nodes;
    QVector<int> stack;
    for (const Item &item : parseItems(n.text)) {
        Branch b;
        b.item = item;
        while (stack.size() > item.level) stack.removeLast();
        b.depth = stack.size();
        nodes << b;
        if (!stack.isEmpty()) nodes[stack.last()].children << nodes.size() - 1;
        stack << nodes.size() - 1;
    }
    // Leaves take one unit each, left to right; a parent centers over its children.
    double next = 0;
    std::function<void(int)> assign = [&](int i) {
        if (nodes[i].children.isEmpty()) {
            nodes[i].slot = next++ + 0.5;
            return;
        }
        for (int child : nodes[i].children) assign(child);
        nodes[i].slot = (nodes[nodes[i].children.first()].slot + nodes[nodes[i].children.last()].slot) / 2;
    };
    for (int i = 0; i < nodes.size(); ++i)
        if (nodes[i].depth == 0) assign(i);
    return nodes;
}
struct TreeMetrics {
    int leaves = 0, levels = 0;
    bool avatars = false;
    qreal box = 0, boxHeight = 0, avatar = 0, gap = 0;
};
TreeMetrics treeMetrics(const Node &n, const QVector<Branch> &nodes, qreal width, Ctx &c) {
    TreeMetrics m;
    const qreal k = scaleOf(n);
    for (const Branch &b : nodes) {
        if (b.children.isEmpty()) ++m.leaves;
        m.levels = qMax(m.levels, b.depth + 1);
        m.avatars = m.avatars || !b.item.image.isEmpty();
    }
    m.avatars = m.avatars && n.prop("avatars") != "none";
    m.gap = c.px(64 * k);
    m.box = qMin(c.px(290 * k), width / qMax(1, m.leaves) - c.px(18));
    m.avatar = m.avatars ? qMin(c.px(92 * k), m.box * 0.42) : 0;
    qreal text = 0;
    for (const Branch &b : nodes) {
        const QString name = b.item.title.isEmpty() ? b.item.text : b.item.title;
        auto title = words(name, 21 * k, n.ink.fg, n.ink.accent, Qt::AlignHCenter, c, true, -1, 1.1);
        qreal h = fitted(*title, m.box - c.px(24), c);
        if (!b.item.title.isEmpty() && !b.item.text.isEmpty()) {
            auto role = words(b.item.text, 16 * k, n.ink.muted, n.ink.accent, Qt::AlignHCenter, c, false, -1, 1.15);
            h += c.px(4) + fitted(*role, m.box - c.px(24), c);
        }
        text = qMax(text, h);
    }
    m.boxHeight = c.px(20 * k) + (m.avatars ? m.avatar + c.px(12 * k) : 0) + text + c.px(20 * k);
    return m;
}
qreal measureTree(Node &n, qreal w, Ctx &c) {
    const QVector<Branch> nodes = branches(n);
    if (nodes.isEmpty()) return 0;
    const TreeMetrics m = treeMetrics(n, nodes, w, c);
    return m.levels * m.boxHeight + (m.levels - 1) * m.gap;
}
void paintTree(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QVector<Branch> nodes = branches(n);
    if (nodes.isEmpty()) return;
    const qreal k = scaleOf(n);
    const TreeMetrics m = treeMetrics(n, nodes, n.rect.width(), c);
    const qreal total = m.levels * m.boxHeight + (m.levels - 1) * m.gap;
    const qreal top = n.rect.top() + qMax(0.0, (n.rect.height() - total) / 2);
    const qreal unit = n.rect.width() / qMax(1, m.leaves);
    auto boxAt = [&](const Branch &b) {
        const qreal x = n.rect.left() + b.slot * unit;
        const qreal y = top + b.depth * (m.boxHeight + m.gap);
        return QRectF(x - m.box / 2, y, m.box, m.boxHeight);
    };
    // Elbow connectors: down from the parent, across, and down into each child.
    const QColor line = mix(n.ink.bg, n.ink.fg, 0.32);
    p->setPen(QPen(line, c.px(2.5), Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    p->setBrush(Qt::NoBrush);
    for (const Branch &b : nodes) {
        if (b.children.isEmpty()) continue;
        const QRectF parent = boxAt(b);
        const qreal mid = parent.bottom() + m.gap / 2;
        p->drawLine(QPointF(parent.center().x(), parent.bottom()), QPointF(parent.center().x(), mid));
        const qreal first = boxAt(nodes[b.children.first()]).center().x(), last = boxAt(nodes[b.children.last()]).center().x();
        p->drawLine(QPointF(qMin(first, parent.center().x()), mid), QPointF(qMax(last, parent.center().x()), mid));
        for (int child : b.children) {
            const QRectF box = boxAt(nodes[child]);
            p->drawLine(QPointF(box.center().x(), mid), QPointF(box.center().x(), box.top()));
        }
    }
    const qreal radius = c.px(c.radius() * 0.8);
    const QColor surface = c.color("surface");
    const Ink inside = inkOver(surface, n.ink, c);
    for (const Branch &b : nodes) {
        const QRectF box = boxAt(b);
        const QPainterPath shape = roundedPath(box, radius, radius, radius, radius);
        if (c.lightSlide) paintShadow(p, box, radius, c, 0.8);
        p->fillPath(shape, surface);
        if (!c.lightSlide) {
            p->setPen(QPen(n.ink.line, c.px(1.5)));
            p->drawPath(shape);
        }
        qreal y = box.top() + c.px(20 * k);
        const QColor tone = b.depth == 0 ? toneOr(n, c) : seriesColor(qMin(b.depth - 1, 5), n.ink, c, colorList(n));
        const QString name = b.item.title.isEmpty() ? b.item.text : b.item.title;
        if (m.avatars) {
            const QRectF circle(box.center().x() - m.avatar / 2, y, m.avatar, m.avatar);
            const QString path = c.asset(b.item.image);
            if (!b.item.image.isEmpty() && QFileInfo::exists(path)) {
                QPainterPath clip;
                clip.addEllipse(circle);
                drawPicture(p, path, circle, clip, "top");
                p->setPen(QPen(tone, c.px(3)));
                p->setBrush(Qt::NoBrush);
                p->drawEllipse(circle.adjusted(c.px(1.5), c.px(1.5), -c.px(1.5), -c.px(1.5)));
            } else
                paintInitials(p, circle, name, tone, c);
            y += m.avatar + c.px(12 * k);
        }
        auto title = words(name, 21 * k, inside.fg, inside.accent, Qt::AlignHCenter, c, true, -1, 1.1);
        const qreal th = fitted(*title, m.box - c.px(24), c);
        drawText(*title, QPointF(box.left() + c.px(12), y), p);
        if (!b.item.title.isEmpty() && !b.item.text.isEmpty()) {
            auto role = words(b.item.text, 16 * k, inside.muted, inside.accent, Qt::AlignHCenter, c, false, -1, 1.15);
            fitted(*role, m.box - c.px(24), c);
            drawText(*role, QPointF(box.left() + c.px(12), y + th + c.px(4)), p);
        }
    }
}

// QR --------------------------------------------------------------------------

struct QrCode {
    QVector<bool> modules;
    int size = 0;
};
QrCode encodeQr(const QString &text) {
    QrCode code;
    QVector<uint8_t> buffer(qrcodegen_BUFFER_LEN_MAX), temp(qrcodegen_BUFFER_LEN_MAX);
    if (!qrcodegen_encodeText(text.toUtf8().constData(), temp.data(), buffer.data(), qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return code;
    code.size = qrcodegen_getSize(buffer.data());
    code.modules.resize(code.size * code.size);
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size; ++x) code.modules[y * code.size + x] = qrcodegen_getModule(buffer.data(), x, y);
    return code;
}
qreal qrSide(const Node &n, const Ctx &c) { return c.px(sizeToken(n.prop("size"), 220)); }
qreal measureQr(Node &n, Ctx &c) {
    qreal h = qrSide(n, c);
    if (n.has("label")) h += c.px(12) + QFontMetricsF(uiFont(c, c.type(18), false, 600)).height();
    return h;
}
void paintQr(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const QrCode code = encodeQr(n.prop("value"));
    if (!code.size) return;
    const qreal side = qrSide(n, c);
    qreal x = n.rect.left() + (n.rect.width() - side) / 2;
    const QString align = n.prop("align", "center");
    if (align == "start" || align == "left") x = n.rect.left();
    else if (align == "end" || align == "right") x = n.rect.right() - side;
    const QRectF plate(x, n.rect.top(), side, side);
    // Scanners need dark modules on a light ground, with a quiet zone around them.
    p->setPen(Qt::NoPen);
    p->setBrush(Qt::white);
    p->drawRoundedRect(plate, side * 0.04, side * 0.04);
    const qreal module = side / (code.size + 4);
    QPainterPath dark;
    for (int y = 0; y < code.size; ++y)
        for (int start = 0; start < code.size;) {
            if (!code.modules[y * code.size + start]) {
                ++start;
                continue;
            }
            int end = start;
            while (end < code.size && code.modules[y * code.size + end]) ++end;
            dark.addRect(QRectF(plate.left() + (start + 2) * module, plate.top() + (y + 2) * module,
                                (end - start) * module, module));
            start = end;
        }
    QColor tone = n.has("tone") ? c.tone(n.prop("tone"), n.ink) : QColor("#111111");
    if (contrast(tone, Qt::white) < 4.5) tone = QColor("#111111");
    p->setPen(QPen(tone, 0.4)); // Hairline seams between modules disappear.
    p->setBrush(tone);
    p->drawPath(dark.simplified());
    if (n.has("label"))
        drawLabel(p, QRectF(x - side * 0.25, plate.bottom() + c.px(12), side * 1.5, n.rect.bottom() - plate.bottom() - c.px(12)),
                  n.prop("label"), uiFont(c, c.type(18), false, 600), n.ink.fg, Qt::AlignHCenter | Qt::AlignTop);
}

// Collage ---------------------------------------------------------------------

QStringList collageFiles(const Node &n) {
    static const QRegularExpression image(R"(!\[[^\]]*\]\((?:<([^>]+)>|([^\s)]+))\))");
    QStringList files;
    for (auto it = image.globalMatch(n.text); it.hasNext();) {
        const auto m = it.next();
        files << (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1));
    }
    return files;
}
QVector<QVector<int>> collageRows(const Node &n, int count) {
    // rows="1,2" gives each row its count; rows="2" balances the pictures over two rows.
    if (n.prop("rows").contains(',')) {
        QVector<QVector<int>> result;
        int next = 0;
        for (const QString &part : n.prop("rows").split(',', Qt::SkipEmptyParts)) {
            QVector<int> row;
            for (int i = 0; i < part.trimmed().toInt() && next < count; ++i) row << next++;
            if (!row.isEmpty()) result << row;
        }
        while (next < count) {
            if (result.isEmpty()) result << QVector<int>();
            result.last() << next++;
        }
        return result;
    }
    int rows = n.prop("rows").toInt();
    if (rows <= 0) rows = count <= 3 ? 1 : count <= 8 ? 2 : 3;
    rows = qBound(1, rows, qMax(1, count));
    QVector<QVector<int>> result(rows);
    for (int i = 0, row = 0; i < count; ++i) {
        // Earlier rows take any extra picture, so the eye starts on the busier row.
        const int perRow = (count + rows - 1 - row) / (rows - row);
        result[row] << i;
        if (result[row].size() >= perRow && row + 1 < rows) ++row;
    }
    return result;
}
qreal collageNatural(const Node &n, qreal w, Ctx &c, QVector<qreal> *heights = nullptr) {
    const QStringList files = collageFiles(n);
    const qreal gap = c.px(n.prop("gap", "12").toDouble());
    qreal total = 0;
    for (const QVector<int> &row : collageRows(n, files.size())) {
        qreal aspect = 0;
        for (int i : row) {
            const QSize size = imageSize(c.asset(files[i]));
            aspect += size.isEmpty() ? 1.5 : double(size.width()) / size.height();
        }
        const qreal h = aspect > 0 ? (w - gap * (row.size() - 1)) / aspect : 0;
        if (heights) *heights << h;
        total += h;
    }
    return total + gap * qMax(0, int(collageRows(n, files.size()).size()) - 1);
}
qreal measureCollage(Node &n, qreal w, Ctx &c) { return qMin(collageNatural(n, w, c), c.px(420)); }
void paintCollage(Node &n, Ctx &c) {
    const QStringList files = collageFiles(n);
    if (files.isEmpty()) return;
    const qreal gap = c.px(n.prop("gap", "12").toDouble());
    QVector<qreal> heights;
    const qreal natural = collageNatural(n, n.rect.width(), c, &heights);
    const QVector<QVector<int>> rows = collageRows(n, files.size());
    const qreal room = n.rect.height() - gap * (rows.size() - 1);
    const qreal scale = natural > 0 ? room / qMax(1.0, natural - gap * (rows.size() - 1)) : 1;
    const qreal radius = n.prop("radius") == "theme" || !n.has("radius") ? c.px(c.radius() * 0.6) : c.px(n.prop("radius").toDouble());
    qreal y = n.rect.top();
    for (int r = 0; r < rows.size(); ++r) {
        const qreal h = heights[r] * scale;
        // Widths keep each picture's proportions, then stretch together to the full width.
        qreal aspect = 0;
        QVector<qreal> shares;
        for (int i : rows[r]) {
            const QSize size = imageSize(c.asset(files[i]));
            shares << (size.isEmpty() ? 1.5 : double(size.width()) / size.height());
            aspect += shares.last();
        }
        qreal x = n.rect.left();
        const qreal width = n.rect.width() - gap * (rows[r].size() - 1);
        for (int j = 0; j < rows[r].size(); ++j) {
            const QRectF cell(x, y, width * shares[j] / aspect, h);
            const QPainterPath clip = roundedPath(cell, radius, radius, radius, radius);
            drawPicture(c.p, c.asset(files[rows[r][j]]), cell, clip, {}, n.flag("grayscale"));
            x += cell.width() + gap;
        }
        y += h + gap;
    }
}

// Video -----------------------------------------------------------------------

// Keyed by a file's path, size and time, so caches forget a replaced file.
QString fileKey(const QString &path) {
    const QFileInfo info(path);
    return path + ':' + QString::number(info.size()) + ':' + QString::number(info.lastModified().toMSecsSinceEpoch());
}
QString videoPoster(const Node &n, const Ctx &c) {
    if (n.has("poster")) return c.deckImage(n.prop("poster"));
    // ensurePoster hashes the video and runs ffmpeg; remember failures too, so a missing
    // ffmpeg or an unreadable file costs that once.
    static QMutex mutex;
    static QHash<QString, QString> posters;
    const QString video = c.video(n.prop("src")), key = fileKey(video);
    {
        QMutexLocker lock(&mutex);
        if (posters.contains(key)) return posters.value(key);
    }
    const QString poster = ensurePoster(video, c.base);
    QMutexLocker lock(&mutex);
    posters.insert(key, poster);
    return poster;
}
// The video's frame size from its header, read once per file; layout must not decode video.
QSize videoSize(const QString &path) {
    static QMutex mutex;
    static QHash<QString, QSize> sizes;
    const QString key = fileKey(path);
    {
        QMutexLocker lock(&mutex);
        if (sizes.contains(key)) return sizes.value(key);
    }
    QSize size;
    if (QFileInfo::exists(path)) {
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of",
                                "csv=p=0:s=x", path});
        if (probe.waitForFinished(5000) && probe.exitCode() == 0) {
            const QStringList parts = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().split('x');
            if (parts.size() == 2) size = QSize(parts[0].toInt(), parts[1].toInt());
        } else
            probe.kill();
    }
    QMutexLocker lock(&mutex);
    sizes.insert(key, size);
    return size;
}
qreal videoAspect(const Node &n, const Ctx &c) {
    const QStringList parts = n.prop("aspect").split(QRegularExpression("[:/x]"));
    if (parts.size() == 2 && parts[1].toDouble() > 0) return parts[0].toDouble() / parts[1].toDouble();
    const QSize size = n.has("poster") ? imageSize(c.deckImage(n.prop("poster"))) : videoSize(c.video(n.prop("src")));
    return size.isEmpty() ? 16.0 / 9 : double(size.width()) / size.height();
}
qreal measureVideo(Node &n, qreal w, Ctx &c) { return w / videoAspect(n, c); }
void paintVideo(Node &n, Ctx &c) {
    QPainter *p = c.p;
    const qreal radius = n.prop("radius") == "theme" ? c.px(c.radius()) : c.px(n.prop("radius").toDouble());
    const QRectF r = n.rect;
    const QPainterPath clip = roundedPath(r, radius, radius, radius, radius);
    if (n.flag("shadow")) paintShadow(p, r, radius, c);
    p->fillPath(clip, Qt::black);
    const QString poster = videoPoster(n, c);
    if (!poster.isEmpty()) drawPicture(p, poster, r, clip);
    // A play button marks it as a video in thumbnails, PDF and PowerPoint previews.
    if (n.flag("play", true)) paintPlay(p, r, n.ink.accent, c);
}

} // namespace

bool isDiagram(const QString &type) { return diagrams.contains(type); }

qreal measureDiagram(Node &n, qreal w, Ctx &c) {
    const QString &t = n.type;
    const qreal fixed = n.has("height") ? -1 : 0;
    Q_UNUSED(fixed);
    if (t == "Chart") {
        const QString type = n.prop("type", "column");
        // Charts grow into free room, so this is only the least they need.
        const qreal base = (type == "bar" || type == "stacked-bar") ? qMax(c.px(220), chartData(n).categories.size() * c.px(56))
                                                                   : w * 0.34;
        return qBound(c.px(220), base, c.px(520));
    }
    if (t == "Progress") {
        const int count = progressBars(n).size();
        return count * progressRow(n, c) + (count - 1) * c.px(22 * scaleOf(n));
    }
    if (t == "Gantt") return measureGantt(n, w, c);
    if (t == "Timeline") return measureTimeline(n, w, c);
    if (t == "Steps") return measureSteps(n, w, c);
    if (t == "Cycle") return measureCycle(n, w, c);
    if (t == "Funnel") return measureFunnel(n, w, c);
    if (t == "Venn") return measureVenn(n, w, c);
    if (t == "Tree") return measureTree(n, w, c);
    if (t == "QR") return measureQr(n, c);
    if (t == "Collage") return measureCollage(n, w, c);
    if (t == "Video") return measureVideo(n, w, c);
    return 0;
}

void paintDiagram(Node &n, Ctx &c) {
    c.p->save();
    const QString &t = n.type;
    if (t == "Chart") paintChart(n, c);
    else if (t == "Progress") paintProgress(n, c);
    else if (t == "Gantt") paintGantt(n, c);
    else if (t == "Timeline") paintTimeline(n, c);
    else if (t == "Steps") paintSteps(n, c);
    else if (t == "Cycle") paintCycle(n, c);
    else if (t == "Funnel") paintFunnel(n, c);
    else if (t == "Venn") paintVenn(n, c);
    else if (t == "Tree") paintTree(n, c);
    else if (t == "QR") paintQr(n, c);
    else if (t == "Collage") paintCollage(n, c);
    else if (t == "Video") paintVideo(n, c);
    c.p->restore();
}

QStringList diagramProblems(const Node &n, const Ctx &c) {
    Q_UNUSED(c);
    const QString &t = n.type;
    if (t == "Chart") {
        const ChartData d = chartData(n);
        if (!d.error.isEmpty()) return {"<Chart> " + d.error};
        if ((n.prop("type") == "pie" || n.prop("type") == "donut") && d.categories.size() > 6)
            return {"A pie or donut reads best with six slices or fewer; use type=\"bar\""};
        if (d.series.size() > 6) return {"<Chart> has more than six series; split it or use a table"};
    }
    if (t == "Gantt") {
        const Schedule s = schedule(n);
        if (!s.error.isEmpty()) return {"<Gantt> " + s.error};
    }
    if (t == "Progress" && n.text.trimmed().isEmpty() && !n.has("value")) return {"<Progress> needs value=\"…\" or a table"};
    if ((t == "Timeline" || t == "Steps" || t == "Cycle" || t == "Funnel") && parseItems(n.text).isEmpty())
        return {"<" + t + "> needs a Markdown list, one item per stage"};
    if (t == "Venn") {
        const int count = parseItems(n.text).size();
        if (count < 2 || count > 4) return {"<Venn> needs two, three or four list items"};
    }
    if (t == "Tree" && parseItems(n.text).isEmpty())
        return {"<Tree> needs a nested Markdown list: indent each person under who they report to"};
    if (t == "QR" && n.has("value") && !encodeQr(n.prop("value")).size) return {"<QR> value is too long to encode"};
    if (t == "Collage" && collageFiles(n).isEmpty()) return {"<Collage> needs images: one ![](photo.jpg) per line"};
    return {};
}
