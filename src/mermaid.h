#pragma once
#include <QHash>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QVariantMap>
class QPainter;
// Mermaid flowcharts, parsed, laid out, and painted natively in the deck's font and colors.
// Why it can't be drawn, or empty when it can. Messages name the Mermaid source line.
QString mermaidProblem(const QString &source);
// Paints the diagram centered in rect, scaled to fit. Returns the size its text reaches in
// rect's units, or 0 when nothing was drawn.
qreal paintMermaid(QPainter *painter, const QRectF &rect, const QString &source,
                   const QVariantMap &palette);
// Node boxes by id, in unscaled layout units, for tests.
QHash<QString, QRectF> mermaidNodeRects(const QString &source,
                                        const QString &font = "JetBrains Mono");
struct MermaidLink {
    QString from, to;
    QPainterPath path;
};
// Drawn links with their ends' ids, in unscaled layout units, for tests.
QList<MermaidLink> mermaidLinks(const QString &source, const QString &font = "JetBrains Mono");
