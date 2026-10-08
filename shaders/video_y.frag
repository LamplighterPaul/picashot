#version 450
// Video output, first plane: brightness (Y) of the filtered picture, BT.709 limited range.
layout(set = 0, binding = 0) uniform sampler2D img;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main() {
    vec3 c = texture(img, v_uv).rgb;
    o_col = vec4(16.0 / 255.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)) * (219.0 / 255.0));
}
