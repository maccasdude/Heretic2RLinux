#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 frag;

layout(set = 0, binding = 0) uniform sampler2D u_diffuse;

void main() {
    frag = texture(u_diffuse, v_uv) * v_color;
}
