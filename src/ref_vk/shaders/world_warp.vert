#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv_diffuse;   // raw texel-space s,t for warp
layout(location = 2) in vec2 in_uv_lightmap;

layout(location = 0) out vec2 v_uv;            // final (warped) UV
layout(location = 1) out vec2 v_uv_lightmap;
layout(location = 2) out float v_fogdist;
layout(location = 3) out float v_alpha;        // params.y, used by the frag

// Per-draw: warp params + model->world matrix. viewproj + fog live in the
// set-1 UBO (so push stays at 96 bytes, within the 128-byte minimum).
layout(push_constant) uniform PC {
    vec4  params;     // x=time, y=alpha, z=flowing(0/1), w=undulate(0/1)
    vec4  params2;    // x=isWarp(0/1), yzw unused
    mat4  model;      // model->world (identity for the static world)
} pc;

#define MAX_DLIGHTS 32
layout(set = 1, binding = 0) uniform Frame {
    mat4  viewproj;
    vec4  fog_cam;
    vec4  fog_color;
    vec4  fog_extra;
    int   count;
    float modulate;
    float _p0, _p1;
    vec4  pos[MAX_DLIGHTS];
    vec4  color[MAX_DLIGHTS];
} fr;

#define TURB_TIME_RATE 0.75
float turb(float coord, float time) { return 8.0 * sin(coord * 0.125 + time * TURB_TIME_RATE); }

void main() {
    vec3 pos = in_pos;
    float time = pc.params.x;

    if (pc.params2.x > 0.5) {
        float os = in_uv_diffuse.x;
        float ot = in_uv_diffuse.y;
        float scroll = 0.0;
        if (pc.params.z > 0.5) {
            float h = time * 0.5;
            scroll = -64.0 * (h - floor(h));
        }
        float s = os + turb(ot, time) + scroll;
        float t = ot + turb(os, time);
        v_uv = vec2(s / 64.0, t / 64.0);

        if (pc.params.w > 0.5) {
            float v0 = in_pos.x * 2.3 + in_pos.y;
            float v1 = in_pos.y * 2.3 + in_pos.x;
            pos.z += 8.0 * sin(v0 * 0.015 + time * TURB_TIME_RATE * 1.5) * 0.25
                   + 8.0 * sin(v1 * 0.015 + time * TURB_TIME_RATE * 3.0) * 0.125;
        }
    } else {
        v_uv = in_uv_diffuse;
    }

    // Displaced vertex -> clip via the per-frame view-projection; fog distance
    // uses the UNdisplaced world position (model * in_pos) against the world cam.
    vec4 wp_disp = pc.model * vec4(pos, 1.0);
    gl_Position   = fr.viewproj * wp_disp;
    v_uv_lightmap = in_uv_lightmap;
    vec3 wp       = (pc.model * vec4(in_pos, 1.0)).xyz;
    v_fogdist     = length(wp - fr.fog_cam.xyz);
    v_alpha       = pc.params.y;
}
