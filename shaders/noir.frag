#version 450
#include "common.glsl"
// High-contrast black and white with film grain.
vec3 effect(vec2 uv) {
    float l = luma(tex(uv));
    l = smoothstep(0.08, 0.92, l);
    l += (hash(uv * RES + fract(TIME) * 91.7) - 0.5) * 0.10;
    return vec3(l) * vignette(uv, 1.1);
}
