#version 450

layout(location = 0) in vec2 v_uv_diffuse;
layout(location = 1) in vec2 v_uv_lightmap;
layout(location = 2) in float v_fogdist;
layout(location = 3) in vec3 v_worldpos;
layout(location = 0) out vec4 frag;

layout(set = 0, binding = 0) uniform sampler2D u_diffuse;
layout(set = 0, binding = 1) uniform sampler2D u_lightmap;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 fog_cam;     // xyz = camera world pos, w = density
    vec4 fog_color;   // rgb = fog color, w = mode (<0 = off)
    vec4 fog_extra;   // x = startdist, y = farclip, z = dlight enable (0/1)
} pc;

#define MAX_DLIGHTS 32
layout(set = 1, binding = 0) uniform DLights {
    int   count;
    float modulate;
    float _p0, _p1;
    vec4  pos[MAX_DLIGHTS];     // xyz origin, w intensity
    vec4  color[MAX_DLIGHTS];   // xyz color 0..1
} dl;

float fog_factor(float dist) {
    float mode = pc.fog_color.w;
    if (mode < 0.0) return 1.0;
    if (mode < 0.5) {
        float start = pc.fog_extra.x;
        float end   = pc.fog_extra.y;
        return clamp((end - dist) / max(end - start, 0.001), 0.0, 1.0);
    } else if (mode < 1.5) {
        float d = pc.fog_cam.w * dist;
        return clamp(exp(-d), 0.0, 1.0);
    } else {
        float d = pc.fog_cam.w * dist;
        return clamp(exp(-d * d), 0.0, 1.0);
    }
}

// Dynamic light contribution in the 0..1 lightmap scale, faithful to ref_gl1
// R_AddDynamicLights + R_BuildLightMap. GL1 accumulates blocklights in a 0..255
// range as (frad)*(color_byte*modulate/255), then converts to a 0..255 byte
// (so /255 again when sampled as 0..1). Net: frad * (color/255) * modulate/255.
vec3 dlight_contribution() {
    vec3 acc = vec3(0.0);
    int n = min(dl.count, MAX_DLIGHTS);
    for (int i = 0; i < n; i++) {
        float intensity = dl.pos[i].w;
        float d = distance(v_worldpos, dl.pos[i].xyz);
        float frad = intensity - d;
        // ref_gl1 uses a 64-unit cutoff (DLIGHT_CUTOFF).
        if (frad > 64.0) {
            // dl.color is 0..255; bring to 0..1 lightmap scale.
            acc += frad * (dl.color[i].xyz / 255.0) * dl.modulate / 255.0;
        }
    }
    return acc;
}

void main() {
    vec4 diff = texture(u_diffuse,  v_uv_diffuse);
    vec3 lm   = texture(u_lightmap, v_uv_lightmap).rgb;
    vec3 light = lm * 2.0;
    if (pc.fog_extra.z > 0.5)
        light += dlight_contribution();

    // ref_gl1 R_BuildLightMap: if the brightest channel exceeds the cap, rescale
    // all three down proportionally (desaturating clamp) rather than clipping
    // each channel - this preserves hue and prevents blowout to white. Cap at
    // 2.0 to match the lm*2.0 headroom used for the static term.
    float peak = max(light.r, max(light.g, light.b));
    if (peak > 2.0)
        light *= 2.0 / peak;

    vec3 col  = diff.rgb * light;

    float f = fog_factor(v_fogdist);
    col = mix(pc.fog_color.rgb, col, f);

    frag = vec4(col, 1.0);
}
