#version 450
#include "common.glsl"
// Four tinted copies, pop-art style.
vec3 effect(vec2 uv) {
    vec2 cell = floor(uv * 2.0);
    float l = luma(tex(fract(uv * 2.0)));
    l = floor(l * 3.0 + 0.5) / 3.0;
    int i = int(cell.x + cell.y * 2.0);
    vec3 dark  = i == 0 ? vec3(0.10, 0.05, 0.45) : i == 1 ? vec3(0.45, 0.02, 0.25) : i == 2 ? vec3(0.02, 0.30, 0.30) : vec3(0.35, 0.10, 0.02);
    vec3 light = i == 0 ? vec3(1.00, 0.85, 0.10) : i == 1 ? vec3(0.30, 0.95, 0.90) : i == 2 ? vec3(1.00, 0.45, 0.65) : vec3(0.75, 1.00, 0.30);
    return mix(dark, light, l);
}
