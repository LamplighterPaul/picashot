// Shared by every filter. A filter is one file that includes this and defines
//     vec3 effect(vec2 uv)
// uv runs 0..1 with (0,0) at the top-left of the picture. See docs/FILTERS.md.
layout(set = 0, binding = 0) uniform sampler2D cam;
layout(push_constant) uniform PC { vec4 a; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

#define RES  pc.a.xy   // camera size in pixels
#define TIME pc.a.z    // seconds since launch

vec3 tex(vec2 uv) { return texture(cam, clamp(uv, 0.0, 1.0)).rgb; }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vignette(vec2 uv, float amount) {
    vec2 d = uv - 0.5;
    return 1.0 - amount * dot(d, d) * 2.0;
}

vec3 effect(vec2 uv);

void main() {
    vec2 uv = v_uv;
    if (pc.a.w > 0.5) uv.x = 1.0 - uv.x;   // mirrored, like a mirror
    o_col = vec4(clamp(effect(uv), 0.0, 1.0), 1.0);
}
