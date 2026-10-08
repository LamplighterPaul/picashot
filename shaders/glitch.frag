#version 450
#include "common.glsl"
// Colour channels pulled apart, scanlines, and the odd torn row.
vec3 effect(vec2 uv) {
    float band = floor(uv.y * 24.0);
    float t = floor(TIME * 9.0);
    float tear = step(0.93, hash(vec2(band, t))) * (hash(vec2(t, band)) - 0.5) * 0.12;
    vec2 u = uv + vec2(tear, 0.0);
    float split = 0.006 + 0.004 * sin(TIME * 3.1);
    vec3 c = vec3(tex(u + vec2(split, 0.0)).r, tex(u).g, tex(u - vec2(split, 0.0)).b);
    c *= 0.88 + 0.12 * sin(uv.y * RES.y * 3.14159);
    return c;
}
