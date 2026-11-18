# Composes one transition into the modern GLSL that qsb bakes for Qt Quick's
# ShaderEffect. The ES 1.00 body stays the single source both renderers compile;
# only the wrapper differs from the HTML exports' WebGL one.
#
#   cmake -DCOMMON=<common.glsl> -DBODY=<name>.glsl -DOUTPUT=<name>.frag -P ...
#
# Qt Quick supplies the vertex shader, whose output is qt_TexCoord0. ShaderEffect
# packs the effect's properties into a std140 block bound at 0, while samplers
# keep their own binding points, so fromTex and toTex match the QML properties of
# those names and progress matches the QML progress property.

foreach(input COMMON BODY)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "wrap-transition: ${input} not found: ${${input}}")
    endif()
endforeach()

file(READ "${COMMON}" common)
file(READ "${BODY}" body)

set(wrapper [=[#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;
};
layout(binding = 1) uniform sampler2D fromTex;
layout(binding = 2) uniform sampler2D toTex;
#define varying in
#define texture2D texture
#define gl_FragColor fragColor
]=])

file(WRITE "${OUTPUT}"
    "${wrapper}\n${common}\n${body}\n\nvoid main() { gl_FragColor = transition(qt_TexCoord0); }\n")
