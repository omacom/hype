#include "transitions.h"
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

// The one list of shaders. "none" is the editor's default: slides swap without
// animating. A new transition is one more name here and one more .glsl file.
QStringList transitionNames() {
    return {QStringLiteral("none"), QStringLiteral("crossfade"), QStringLiteral("dissolve"),
            QStringLiteral("liquid"), QStringLiteral("glitch")};
}

static QString readTransitionResource(const QString &name) {
    QFile file(":/transitions/" + name + ".glsl");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// Each body is shared, so both platforms compile the identical GLSL. The helpers
// are prepended once per body, which keeps every shader self-contained.
static QString composeBody(const QString &name) {
    if (name == "none" || !transitionNames().contains(name))
        return {};
    const QString common = readTransitionResource("common");
    const QString body = readTransitionResource(name);
    if (common.isEmpty() || body.isEmpty())
        return {};
    return common + "\n" + body;
}

QString transitionBody(const QString &name) {
    static QMutex mutex;
    static QHash<QString, QString> cache;
    QMutexLocker locker(&mutex);
    const auto cached = cache.constFind(name);
    if (cached != cache.cend())
        return *cached;
    const QString body = composeBody(name);
    cache.insert(name, body);
    return body;
}

QString qtFragmentShader(const QString &name) {
    static QMutex mutex;
    static QHash<QString, QString> cache;
    QMutexLocker locker(&mutex);
    const auto cached = cache.constFind(name);
    if (cached != cache.cend())
        return *cached;
    const QString body = transitionBody(name);
    QString shader;
    if (!body.isEmpty()) {
        shader = QStringLiteral("uniform sampler2D fromTex;\n"
                                "uniform sampler2D toTex;\n"
                                "uniform float progress;\n"
                                "varying vec2 qt_TexCoord0;\n\n") +
                 body +
                 QStringLiteral("\nvoid main() {\n"
                                "    gl_FragColor = transition(qt_TexCoord0);\n"
                                "}\n");
    }
    cache.insert(name, shader);
    return shader;
}

QByteArray transitionsJson() {
    QJsonObject shaders;
    for (const QString &name : transitionNames()) {
        const QString body = transitionBody(name);
        if (!body.isEmpty())
            shaders.insert(name, body);
    }
    QByteArray json = QJsonDocument(shaders).toJson(QJsonDocument::Compact);
    // JSON leaves '<' alone; in a <script> block it could spell the end tag.
    return json.replace('<', "\\u003c");
}
