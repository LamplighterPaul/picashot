#version 450
#include "common.glsl"
// The opposite of Bulge: the middle is squeezed away from you.
vec3 effect(vec2 uv) {
    vec2 aspect = vec2(RES.x / RES.y, 1.0);
    vec2 d = (uv - 0.5) * aspect;
    float r = length(d);
    float radius = 0.5;
    if (r < radius) {
        float n = r / radius;
        d *= mix(1.9, 1.0, smoothstep(0.0, 1.0, n));
    }
    return tex(d / aspect + 0.5);
}
