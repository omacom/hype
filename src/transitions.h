#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

// Slide transitions, each one a GLSL ES 1.00 fragment body:
//
//     vec4 transition(vec2 uv)
//
// The body samples fromTex and toTex at uv and mixes them by progress, so the
// same source runs in the Qt Quick app (ShaderEffect) and in the HTML exports
// (WebGL 1). The two wrappers below add only the platform's preamble and main.
QStringList transitionNames();
// The shared helpers followed by the named body, or empty for "none" and
// names that have no shader.
QString transitionBody(const QString &name);
// The body wrapped as a ShaderEffect fragment shader.
QString qtFragmentShader(const QString &name);
// Every body keyed by name, as compact JSON for the exported page's WebGL
// renderer. "none" is absent: it means the slides change without a shader.
// '<' is escaped as JSON's < so the block cannot close its <script>.
QByteArray transitionsJson();
