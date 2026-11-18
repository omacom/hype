// The baseline: the two slides simply overlap.

vec4 transition(vec2 uv) {
    vec4 from = texture2D(fromTex, uv);
    vec4 to = texture2D(toTex, uv);
    return mix(from, to, progress);
}
