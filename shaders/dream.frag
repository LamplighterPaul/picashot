#version 450
#include "common.glsl"
// Soft glow: bright areas bleed into their surroundings.
vec3 effect(vec2 uv) {
    vec3 c = tex(uv);
    vec3 glow = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 16; i++) {
        float a = float(i) * 2.39996;
        float r = sqrt(float(i) + 0.5) / 4.0;
        vec2 o = vec2(cos(a), sin(a)) * r * 0.035 * vec2(RES.y / RES.x, 1.0);
        vec3 s = tex(uv + o);
        float w = 1.0 - r * 0.5;
        glow += max(s - 0.45, 0.0) * w;
        total += w;
    }
    glow /= total;
    c = c + glow * 1.6;
    c = mix(c, c * vec3(1.06, 0.98, 1.08), 0.6);
    return c * vignette(uv, 0.5);
}
