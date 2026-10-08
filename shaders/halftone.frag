#version 450
#include "common.glsl"
// Newsprint dots.
vec3 effect(vec2 uv) {
    float cells = 130.0;
    vec2 p = uv * vec2(cells, cells * RES.y / RES.x);
    mat2 rot = mat2(0.7071, -0.7071, 0.7071, 0.7071);
    vec2 q = rot * p;
    vec2 cell = floor(q) + 0.5;
    vec2 cuv = (transpose(rot) * cell) / vec2(cells, cells * RES.y / RES.x);
    float l = luma(tex(cuv));
    float radius = sqrt(1.0 - smoothstep(0.05, 0.85, l)) * 0.70;
    float d = length(q - cell);
    float ink = 1.0 - smoothstep(radius - 0.08, radius + 0.08, d);
    return mix(vec3(0.96, 0.94, 0.88), vec3(0.08, 0.07, 0.10), ink);
}
