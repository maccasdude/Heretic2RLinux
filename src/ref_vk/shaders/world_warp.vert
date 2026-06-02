#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv_diffuse;   // raw texel-space s,t for warp
layout(location = 2) in vec2 in_uv_lightmap;

layout(location = 0) out vec2 v_uv;            // final (warped) UV
layout(location = 1) out vec2 v_uv_lightmap;
layout(location = 2) out float v_fogdist;

layout(push_constant) uniform PC {
    mat4  mvp;
    vec4  params;     // x=time, y=alpha, z=flowing(0/1), w=undulate(0/1)
    vec4  params2;    // x=isWarp(0/1), yzw unused
    vec4  fog_cam;    // xyz = camera world pos, w = density
    vec4  fog_color;  // rgb = fog color, w = mode (<0 = off)
    vec4  fog_extra;  // x = startdist, y = farclip
} pc;

// ref_gl1 turbsin warp. The churn/turbulence comes from the SPATIAL term
// (coord*0.125) varying per-vertex across the subdivided grid, with s offset by
// turb(ot) and t by turb(os) (cross-coupling). Using the raw coord directly
// (radians) gives the correct dense churn; the degree-scaled version collapsed
// the spatial variation so the whole surface slid as one rigid sheet (wrong).
// We keep the spatial term raw (faithful churn) but slow the TIME phase to a
// pleasant rate (the old raw version animated correctly but too fast).
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

    gl_Position   = pc.mvp * vec4(pos, 1.0);
    v_uv_lightmap = in_uv_lightmap;
    v_fogdist     = length(in_pos - pc.fog_cam.xyz);
}
