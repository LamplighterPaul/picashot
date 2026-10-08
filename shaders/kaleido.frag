#version 450
#include "common.glsl"
// The left half reflected onto the right.
vec3 effect(vec2 uv) {
    uv.x = 0.5 - abs(uv.x - 0.5);
    return tex(uv);
}
