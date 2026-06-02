#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv_diffuse;
layout(location = 2) in vec2 in_uv_lightmap;

layout(location = 0) out vec2 v_uv_diffuse;
layout(location = 1) out vec2 v_uv_lightmap;
layout(location = 2) out float v_fogdist;
layout(location = 3) out vec3 v_worldpos;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 fog_cam;     // xyz = camera world pos, w = density
    vec4 fog_color;   // rgb = fog color, w = mode (<0 = off)
    vec4 fog_extra;   // x = startdist, y = farclip
} pc;

void main() {
    gl_Position   = pc.mvp * vec4(in_pos, 1.0);
    v_uv_diffuse  = in_uv_diffuse;
    v_uv_lightmap = in_uv_lightmap;
    v_fogdist     = length(in_pos - pc.fog_cam.xyz);
    v_worldpos    = in_pos;
}
