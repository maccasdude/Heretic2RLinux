#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_uv_lightmap;
layout(location = 2) in float v_fogdist;
layout(location = 0) out vec4 frag;

layout(set = 0, binding = 0) uniform sampler2D u_diffuse;
layout(set = 0, binding = 1) uniform sampler2D u_lightmap;

layout(push_constant) uniform PC {
    mat4  mvp;
    vec4  params;
    vec4  params2;
    vec4  fog_cam;
    vec4  fog_color;
    vec4  fog_extra;
} pc;

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

void main() {
    vec4 diff = texture(u_diffuse, v_uv);
    vec3 col = diff.rgb;
    float f = fog_factor(v_fogdist);
    col = mix(pc.fog_color.rgb, col, f);
    frag = vec4(col, diff.a * pc.params.y);
}
