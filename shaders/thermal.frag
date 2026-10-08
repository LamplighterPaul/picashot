#version 450
#include "common.glsl"
// Brightness mapped to a heat-camera palette.
vec3 effect(vec2 uv) {
    float l = luma(tex(uv));
    vec3 c = mix(vec3(0.0, 0.0, 0.25), vec3(0.3, 0.0, 0.7), smoothstep(0.0, 0.25, l));
    c = mix(c, vec3(0.95, 0.1, 0.3), smoothstep(0.25, 0.5, l));
    c = mix(c, vec3(1.0, 0.7, 0.0), smoothstep(0.5, 0.75, l));
    c = mix(c, vec3(1.0, 1.0, 0.85), smoothstep(0.75, 1.0, l));
    return c;
}
