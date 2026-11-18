// A threshold dissolve: the incoming slide eats through the outgoing one along
// a field of soft, grainy noise, with a thin luminous rim at the melting edge.

vec4 transition(vec2 uv) {
    // Broad organic motion, roughened at a fine scale so the edge never reads as
    // a smooth gradient.
    float field = snoise(uv * 8.0 + 3.7) * 0.5 + 0.5;
    field += (hash21(floor(uv * 180.0)) - 0.5) * 0.16;
    field = clamp(field, 0.0, 1.0);

    // Overshoot both ends so the first and last frames are complete.
    float edge = progress * 1.12 - 0.06;
    float dissolve = smoothstep(field - 0.055, field + 0.055, edge);
    vec4 color = mix(texture2D(fromTex, uv), texture2D(toTex, uv), dissolve);

    // A warm band glows where the noise threshold crosses the sweep.
    float rim = smoothstep(0.11, 0.0, abs(edge - field));
    rim *= 1.0 - abs(progress * 2.0 - 1.0);
    vec3 glow = palette(field * 0.8 + progress * 0.25,
                        vec3(0.5), vec3(0.5), vec3(1.0), vec3(0.0, 0.33, 0.67));
    color.rgb += glow * rim * 0.38;
    return color;
}
