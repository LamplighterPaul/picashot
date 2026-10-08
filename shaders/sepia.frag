#version 450
#include "common.glsl"
vec3 effect(vec2 uv) {
    vec3 c = tex(uv);
    vec3 s = vec3(dot(c, vec3(0.393, 0.769, 0.189)), dot(c, vec3(0.349, 0.686, 0.168)), dot(c, vec3(0.272, 0.534, 0.131)));
    return mix(c, s, 0.9) * vignette(uv, 0.9);
}
