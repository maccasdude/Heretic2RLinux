#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv_diffuse;
layout(location = 2) in vec2 in_uv_lightmap;

layout(location = 0) out vec2 v_uv_diffuse;
layout(location = 1) out vec2 v_uv_lightmap;
layout(location = 2) out float v_fogdist;
layout(location = 3) out vec3 v_worldpos;

// Per-draw: only the model->world matrix. Identity for the static world; for
// inline submodels it is T(origin)*R(angles). Keeping just this in push
// constants (64 bytes) stays within Vulkan's 128-byte guaranteed minimum.
layout(push_constant) uniform PC {
    mat4 model;
} pc;

// Per-frame data (set = 1): view-projection + fog (+ the dynamic lights the
// fragment shader uses). std140; must byte-match dlight_ubo_t in C.
#define MAX_DLIGHTS 32
layout(set = 1, binding = 0) uniform Frame {
    mat4  viewproj;
    vec4  fog_cam;     // xyz = camera world pos, w = density
    vec4  fog_color;   // rgb = fog color, w = mode (<0 = off)
    vec4  fog_extra;   // x = startdist, y = farclip, z = dlight enable (0/1)
    int   count;
    float modulate;
    float _p0, _p1;
    vec4  pos[MAX_DLIGHTS];
    vec4  color[MAX_DLIGHTS];
} fr;

void main() {
    // World position of this vertex (model = identity for the static world, so
    // this is just in_pos there). Used for clip pos, fog distance and the
    // world-space dynamic-light test in the fragment shader.
    vec4 wp = pc.model * vec4(in_pos, 1.0);

    gl_Position   = fr.viewproj * wp;
    v_uv_diffuse  = in_uv_diffuse;
    v_uv_lightmap = in_uv_lightmap;
    v_worldpos    = wp.xyz;
    v_fogdist     = length(wp.xyz - fr.fog_cam.xyz);
}
