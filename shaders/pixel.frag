#version 450
#include "common.glsl"
// Chunky pixels with a reduced palette.
vec3 effect(vec2 uv) {
    float cells = 96.0;
    vec2 grid = vec2(cells, cells * RES.y / RES.x);
    vec3 c = tex((floor(uv * grid) + 0.5) / grid);
    return floor(c * 6.0 + 0.5) / 6.0;
}
