#version 450
// Turns the camera's YUV planes into the RGBA picture the filters read. Runs once per camera frame.
layout(set = 0, binding = 0) uniform sampler2D plane0;   // Y, or Y+chroma bytes for YUYV
layout(set = 0, binding = 1) uniform sampler2D plane1;   // U, or U and V together for NV12
layout(set = 0, binding = 2) uniform sampler2D plane2;   // V for three-plane layouts
layout(push_constant) uniform PC {
    vec4 a;   // layout (1 NV12, 2 YUYV, 3 three planes), 1 if full range, 1 if BT.709, unused
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    int kind = int(pc.a.x);
    float y;
    vec2 uv;
    if (kind == 2) {
        // Y,U,Y,V: every pixel has its own Y; each pair of pixels shares one U and one V.
        y = texelFetch(plane0, px, 0).r;
        int even = px.x & ~1;
        uv = vec2(texelFetch(plane0, ivec2(even, px.y), 0).g, texelFetch(plane0, ivec2(even + 1, px.y), 0).g);
    } else {
        y = texelFetch(plane0, px, 0).r;
        uv = kind == 1 ? texture(plane1, v_uv).rg : vec2(texture(plane1, v_uv).r, texture(plane2, v_uv).r);
    }
    if (pc.a.y > 0.5) {
        uv -= 128.0 / 255.0;
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        uv = (uv - 128.0 / 255.0) * (255.0 / 224.0);
    }
    vec3 rgb = pc.a.z > 0.5
        ? vec3(y + 1.5748 * uv.y, y - 0.1873 * uv.x - 0.4681 * uv.y, y + 1.8556 * uv.x)
        : vec3(y + 1.402 * uv.y, y - 0.344136 * uv.x - 0.714136 * uv.y, y + 1.772 * uv.x);
    o_col = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
