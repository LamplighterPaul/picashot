#version 450
// Video output, second plane: colour (U and V) at half size. The linear sampler averages each 2x2 block.
layout(set = 0, binding = 0) uniform sampler2D img;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main() {
    vec3 c = texture(img, v_uv).rgb;
    float u = 128.0 / 255.0 + dot(c, vec3(-0.1146, -0.3854, 0.5)) * (224.0 / 255.0);
    float v = 128.0 / 255.0 + dot(c, vec3(0.5, -0.4542, -0.0458)) * (224.0 / 255.0);
    o_col = vec4(u, v, 0.0, 1.0);
}
