#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_normal;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;
layout(location = 2) out float v_fogdist;
layout(location = 3) out vec3 v_worldpos;
layout(location = 4) out vec3 v_normal;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 tint;
    vec4 fog_cam;     // xyz = camera world pos
    vec4 fog_color;
    vec4 fog_extra;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_uv   = in_uv;
    v_tint = pc.tint;
    v_fogdist  = length(in_pos - pc.fog_cam.xyz);
    v_worldpos = in_pos;
    v_normal   = in_normal;
}
