// Blocky horizontal displacement with an RGB split, like a signal dropping out.
// The corruption peaks mid-transition and is gone on both ends.

vec4 transition(vec2 uv) {
    // 0 at the ends, 1 halfway through.
    float intensity = sin(progress * 3.14159265);
    float blend = smoothstep(0.0, 1.0, progress);

    // Each band of the frame jumps by its own amount, re-rolled a few times
    // during the transition rather than every frame.
    float band = floor(uv.y * 15.0);
    float roll = floor(progress * 20.0);
    float noise = hash21(vec2(band, roll));
    float shift = (noise - 0.5) * 0.11 * intensity;

    vec2 displaced = vec2(shift, 0.0);
    vec4 from = texture2D(fromTex, clamp(uv + displaced, 0.0, 1.0));
    vec4 to = texture2D(toTex, clamp(uv + displaced, 0.0, 1.0));
    vec4 color = mix(from, to, clamp(blend + (noise - 0.5) * 0.45 * intensity, 0.0, 1.0));

    // Split the channels in opposite directions; the incoming slide's red and
    // the outgoing slide's blue trail the cut.
    vec2 split = vec2(0.022, 0.0) * intensity * (0.5 + noise);
    color.r = mix(color.r,
                  texture2D(toTex, clamp(uv + displaced + split, 0.0, 1.0)).r,
                  intensity * 0.75);
    color.b = mix(color.b,
                  texture2D(fromTex, clamp(uv + displaced - split, 0.0, 1.0)).b,
                  intensity * 0.75);

    // Occasional scanlines and a pale flash at the peak sell the dropout.
    float scan = step(0.5, fract(uv.y * 120.0 + progress * 7.0));
    color.rgb += vec3(0.10) * scan * intensity;
    color.rgb += vec3(0.06) * intensity * intensity;
    return color;
}
