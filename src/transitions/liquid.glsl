// A domain-warped wipe in the spirit of rainbowshader.goo: two layers of simplex
// noise bend a dissolve front, so the new slide flows in rather than slides.

vec4 transition(vec2 uv) {
    // Keep the noise isotropic on Hype's 16:9 slide.
    vec2 p = uv * vec2(2.6, 1.46);
    float drift = progress * 2.4;

    // First warp: where the liquid is heading.
    vec2 q = vec2(snoise(p + vec2(0.0, drift)),
                  snoise(p + vec2(5.2, 1.3) + vec2(0.0, drift)));

    // Second warp: how sharply it folds.
    vec2 r = vec2(snoise(p + 2.0 * q + vec2(1.7, 9.2) + 0.3 * drift),
                  snoise(p + 2.0 * q + vec2(8.3, 2.8) + 0.26 * drift));

    float field = snoise(p + 2.5 * r) * 0.5 + 0.5;

    // A diagonal sweep biased by the warped field. The mask stays in [0, 1], and
    // the edge travels past both ends, so progress 0 and 1 are fully settled.
    float mask = clamp(field * 0.62 + (uv.x * 0.55 + uv.y * 0.45) * 0.52, 0.0, 1.0);
    float softness = 0.16;
    float edge = progress * (1.0 + 2.0 * softness) - softness;
    float wipe = smoothstep(mask - softness, mask + softness, edge);

    // The slides themselves ripple at the front, then settle.
    vec2 wobble = r * 0.012 * (1.0 - abs(progress * 2.0 - 1.0));
    vec4 from = texture2D(fromTex, clamp(uv + wobble, 0.0, 1.0));
    vec4 to = texture2D(toTex, clamp(uv - wobble, 0.0, 1.0));
    vec4 color = mix(from, to, wipe);

    // A rainbow rim rides the front and fades as the wipe completes.
    float rim = smoothstep(0.14, 0.0, abs(edge - mask));
    rim *= sin(progress * 3.14159265);
    vec3 glow = palette(field * 0.7 + length(q) * 0.15 + progress * 0.4,
                        vec3(0.5), vec3(0.5), vec3(1.0), vec3(0.0, 0.33, 0.67));
    color.rgb += glow * rim * 0.42;
    return color;
}
