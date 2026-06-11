#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_uv_lightmap;
layout(location = 2) in float v_fogdist;
layout(location = 3) in float v_alpha;
layout(location = 0) out vec4 frag;

layout(set = 0, binding = 0) uniform sampler2D u_diffuse;
layout(set = 0, binding = 1) uniform sampler2D u_lightmap;

// Per-frame data (set = 1): only fog is used here (warp surfaces are fullbright
// and ignore the dynamic lights). std140; matches the 'Frame' block elsewhere.
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

float fog_factor(float dist) {
    float mode = fr.fog_color.w;
    if (mode < 0.0) return 1.0;
    if (mode < 0.5) {
        float start = fr.fog_extra.x;
        float end   = fr.fog_extra.y;
        return clamp((end - dist) / max(end - start, 0.001), 0.0, 1.0);
    } else if (mode < 1.5) {
        float d = fr.fog_cam.w * dist;
        return clamp(exp(-d), 0.0, 1.0);
    } else {
        float d = fr.fog_cam.w * dist;
        return clamp(exp(-d * d), 0.0, 1.0);
    }
}

void main() {
    vec4 diff = texture(u_diffuse, v_uv);
    vec3 col = diff.rgb;
    float f = fog_factor(v_fogdist);
    col = mix(fr.fog_color.rgb, col, f);
    frag = vec4(col, diff.a * v_alpha);
}
