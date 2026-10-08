#version 450
#include "common.glsl"
// Glowing coloured outlines on black.
vec3 effect(vec2 uv) {
    vec2 px = 1.5 / RES;
    vec3 gx = tex(uv + vec2(px.x, 0.0)) - tex(uv - vec2(px.x, 0.0));
    vec3 gy = tex(uv + vec2(0.0, px.y)) - tex(uv - vec2(0.0, px.y));
    float e = smoothstep(0.05, 0.45, length(gx) + length(gy));
    vec3 hue = 0.5 + 0.5 * cos(6.2832 * (uv.x + uv.y * 0.6 + TIME * 0.08 + vec3(0.0, 0.33, 0.67)));
    return hue * e * 1.6 + tex(uv) * 0.08;
}
