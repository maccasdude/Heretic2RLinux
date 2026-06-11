#version 450

layout(location = 0) in vec2 v_uv_diffuse;
layout(location = 1) in vec2 v_uv_lightmap;
layout(location = 2) in float v_fogdist;
layout(location = 3) in vec3 v_worldpos;
layout(location = 0) out vec4 frag;

layout(set = 0, binding = 0) uniform sampler2D u_diffuse;
layout(set = 0, binding = 1) uniform sampler2D u_lightmap;

// Per-frame data (set = 1): view-projection (unused here) + fog + dynamic lights.
// std140; must byte-match dlight_ubo_t in C and the 'Frame' block in world.vert.
#define MAX_DLIGHTS 32
layout(set = 1, binding = 0) uniform Frame {
    mat4  viewproj;
    vec4  fog_cam;     // xyz = camera world pos, w = density
    vec4  fog_color;   // rgb = fog color, w = mode (<0 = off)
    vec4  fog_extra;   // x = startdist, y = farclip, z = dlight enable (0/1)
    int   count;
    float modulate;
    float _p0, _p1;
    vec4  pos[MAX_DLIGHTS];     // xyz origin, w intensity
    vec4  color[MAX_DLIGHTS];   // xyz color 0..255
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

// Dynamic light contribution in the 0..1 lightmap scale, faithful to ref_gl1
// R_AddDynamicLights + R_BuildLightMap: frad * (color/255) * modulate/255.
vec3 dlight_contribution() {
    vec3 acc = vec3(0.0);
    int n = min(fr.count, MAX_DLIGHTS);
    for (int i = 0; i < n; i++) {
        float intensity = fr.pos[i].w;
        float d = distance(v_worldpos, fr.pos[i].xyz);
        float frad = intensity - d;
        if (frad > 64.0)   // ref_gl1 DLIGHT_CUTOFF
            acc += frad * (fr.color[i].xyz / 255.0) * fr.modulate / 255.0;
    }
    return acc;
}

void main() {
    vec4 diff = texture(u_diffuse,  v_uv_diffuse);
    vec3 lm   = texture(u_lightmap, v_uv_lightmap).rgb;
    vec3 light = lm * 2.0;
    if (fr.fog_extra.z > 0.5)
        light += dlight_contribution();

    // ref_gl1 desaturating clamp: rescale all channels if the peak exceeds the
    // 2.0 headroom rather than clipping each (preserves hue, avoids white blowout).
    float peak = max(light.r, max(light.g, light.b));
    if (peak > 2.0)
        light *= 2.0 / peak;

    vec3 col = diff.rgb * light;

    float f = fog_factor(v_fogdist);
    col = mix(fr.fog_color.rgb, col, f);

    frag = vec4(col, 1.0);
}
