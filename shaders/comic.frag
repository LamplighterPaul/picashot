#version 450
#include "common.glsl"
// Flat colour bands with inked edges.
vec3 effect(vec2 uv) {
    vec2 px = 1.0 / RES;
    // Smooth first, so the colour bands follow shapes and not camera noise.
    vec3 c = vec3(0.0);
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
            c += tex(uv + vec2(x, y) * px * 2.0);
    c /= 25.0;

    vec2 e = px * 2.0;
    float tl = luma(tex(uv + e * vec2(-1, -1))), t = luma(tex(uv + e * vec2(0, -1))), tr = luma(tex(uv + e * vec2(1, -1)));
    float l = luma(tex(uv + e * vec2(-1, 0))), r = luma(tex(uv + e * vec2(1, 0)));
    float bl = luma(tex(uv + e * vec2(-1, 1))), b = luma(tex(uv + e * vec2(0, 1))), br = luma(tex(uv + e * vec2(1, 1)));
    float gx = -tl - 2.0 * l - bl + tr + 2.0 * r + br;
    float gy = -tl - 2.0 * t - tr + bl + 2.0 * b + br;
    float edge = smoothstep(0.35, 0.8, length(vec2(gx, gy)));

    float lum = luma(c);
    float banded = (floor(lum * 5.0) + 0.5) / 5.0;
    c = mix(vec3(lum), c, 1.35) * (banded / max(lum, 0.02));
    return mix(c, vec3(0.06, 0.05, 0.08), edge);
}
