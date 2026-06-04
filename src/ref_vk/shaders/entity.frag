#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;
layout(location = 2) in float v_fogdist;
layout(location = 3) in vec3 v_worldpos;
layout(location = 0) out vec4 frag;

layout(set = 0, binding = 0) uniform sampler2D u_diffuse;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 tint;
    vec4 fog_cam;
    vec4 fog_color;
    vec4 fog_extra;   // z = dlight enable
} pc;

#define MAX_DLIGHTS 32
layout(set = 1, binding = 0) uniform DLights {
    int   count;
    float modulate;
    float _p0, _p1;
    vec4  pos[MAX_DLIGHTS];     // xyz origin, w intensity
    vec4  color[MAX_DLIGHTS];   // xyz color 0..255
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

// Dynamic light add in 0..1 scale (matches world.frag / ref_gl1).
vec3 dlight_contribution() {
    vec3 acc = vec3(0.0);
    int n = min(dl.count, MAX_DLIGHTS);
    for (int i = 0; i < n; i++) {
        float intensity = dl.pos[i].w;
        float d = distance(v_worldpos, dl.pos[i].xyz);
        float frad = intensity - d;
        if (frad > 64.0)
            acc += frad * (dl.color[i].xyz / 255.0) * dl.modulate / 255.0;
    }
    return acc;
}

void main() {
    vec4 c = texture(u_diffuse, v_uv) * v_tint;

    bool is_sky = (pc.fog_extra.w > 0.5);
    // fog_extra.w < -0.5 means "do not alpha-test this draw". ref_gl1 draws flex
    // models with NO alpha test (gl1_FlexModel.c has no glAlphaFunc), so an
    // opaque model whose skin has a band of fully-transparent palette texels
    // (index 255 in an .m8 -> alpha 0) must still draw that band opaque. The
    // global discard below is only correct for cutout sprites/decals/alpha-
    // textured surfaces, which set w >= -0.5.
    bool no_alpha_test = (pc.fog_extra.w < -0.5);

    // Alpha test (matches ref_gl1 R_AlphaFunc GL_GREATER ~0.05 for sprites and
    // alpha-textured surfaces): discard near-transparent texels so cutout
    // sprites (vines, plant cards) and decals don't draw opaque quads where the
    // texture is transparent. Harmless for fully-opaque textures (alpha 1).
    // NEVER alpha-test the sky, and never alpha-test plain flex models.
    if (!is_sky && !no_alpha_test && c.a < 0.05) discard;

    // Models are pre-lit by the engine's shadelight baked into v_tint. Add
    // dynamic light on top when enabled, with the same desaturating cap as the
    // world so nearby lights don't blow models out to white.
    if (pc.fog_extra.z > 0.5) {
        vec3 add = dlight_contribution();
        vec3 lit = c.rgb + texture(u_diffuse, v_uv).rgb * add;
        float peak = max(lit.r, max(lit.g, lit.b));
        if (peak > 2.0) lit *= 2.0 / peak;
        c.rgb = lit;
    }

    // Sky fog: the sky geometry sits ~1 unit from the camera so a distance-based
    // factor wouldn't fog it, but ref_gl1 fogs the sky into the horizon haze.
    // For the sky, force the full fog colour ONLY when fog is actually active
    // (mode = fog_color.w >= 0); with fog off, fog_factor returns 1.0 so the sky
    // shows through unchanged. Non-sky surfaces use the normal distance factor.
    float mode = pc.fog_color.w;
    float f;
    if (is_sky)
        f = (mode < 0.0) ? 1.0 : 0.0;   // fog off -> show sky; fog on -> full haze
    else
        f = fog_factor(v_fogdist);
    c.rgb = mix(pc.fog_color.rgb, c.rgb, f);
    frag = c;
}
