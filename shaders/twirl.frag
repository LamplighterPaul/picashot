#version 450
#include "common.glsl"
vec3 effect(vec2 uv) {
    vec2 aspect = vec2(RES.x / RES.y, 1.0);
    vec2 d = (uv - 0.5) * aspect;
    float r = length(d);
    float radius = 0.5;
    float k = max(0.0, 1.0 - r / radius);
    float a = k * k * (2.6 + 0.5 * sin(TIME * 1.3));
    float s = sin(a), c = cos(a);
    d = mat2(c, -s, s, c) * d;
    return tex(d / aspect + 0.5);
}
