#version 450
#include "common.glsl"
vec3 effect(vec2 uv) {
    float l = 1.0 - luma(tex(uv));
    l = pow(l, 1.4);
    return vec3(l * 0.75, l * 0.95, l * 1.05) * vignette(uv, 0.8);
}
