#version 450

// 3D debug primitives (boxes/lines/arrows/markers) drawn as a single LINE_LIST.
// Per-vertex color; MVP via push constant. Matches ref_gl1 gl1_Debug.c, which
// draws with depth test off so primitives are always visible.

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 v_color;

layout(push_constant) uniform PC {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_color     = in_color;
}
