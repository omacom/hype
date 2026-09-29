#pragma once
#include <QString>
#include <QTextFormat>
#include <QVector>
class QColor;
class QPainter;
class QTextDocument;

// A math span in Markdown source. `start`/`length` cover the delimiters.
// `tex` is the formula with surrounding whitespace removed.
struct MathSpan {
    int start = 0, length = 0;
    QString tex;
    bool display = false;
};

// Obsidian's dollar rules, so prices are not formulas. See math.cpp.
QVector<MathSpan> findMath(const QString &markdown);

// Replace spans with placeholders, then turn those into paintable objects after
// QTextDocument::setMarkdown. `color` is the starting ink; layout may recolor.
QString markdownWithMath(const QString &markdown, const QVector<MathSpan> &spans);
void materializeMath(QTextDocument &document, const QVector<MathSpan> &spans, const QColor &color);

// Object type used on the replacement character, so slide layout can center
// a displayed equation without centering the whole slide.
constexpr int HypeMathObject = QTextFormat::UserObject + 1;
constexpr int HypeMathTex = QTextFormat::UserProperty + 1;
constexpr int HypeMathDisplay = QTextFormat::UserProperty + 2;
