#version 450
#include "common.glsl"
// Fisheye: the middle of the picture swells towards you.
vec3 effect(vec2 uv) {
    vec2 aspect = vec2(RES.x / RES.y, 1.0);
    vec2 d = (uv - 0.5) * aspect;
    float r = length(d);
    float radius = 0.48;
    if (r < radius) {
        float n = r / radius;
        d *= mix(n * n * 0.55 + 0.45 * n, 1.0, n);
    }
    return tex(d / aspect + 0.5);
}
