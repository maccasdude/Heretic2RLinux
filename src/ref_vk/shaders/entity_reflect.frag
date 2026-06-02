#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;
layout(location = 2) in float v_fogdist;
layout(location = 3) in vec3 v_worldpos;
layout(location = 4) in vec3 v_normal;
layout(location = 0) out vec4 frag;

// set 0 binding 0 is bound to the reflect texture (misc/reflect.m32) instead of
// the skin for reflective entities.
layout(set = 0, binding = 0) uniform sampler2D u_reflect;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 tint;
    vec4 fog_cam;     // xyz = camera world pos
    vec4 fog_color;   // w = mode (<0 = off)
    vec4 fog_extra;
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
    // Classic GL_SPHERE_MAP environment mapping. Reflect the eye->fragment
    // direction about the surface normal, then map the reflection vector to a
    // sphere-map UV. Done in world space relative to the camera, which gives
    // the same chrome look as fixed-function sphere mapping.
    vec3 n = normalize(v_normal);
    vec3 eye = normalize(v_worldpos - pc.fog_cam.xyz);
    vec3 r = reflect(eye, n);
    // GL_SPHERE_MAP: m = 2*sqrt(rx^2 + ry^2 + (rz+1)^2); uv = r.xy/m + 0.5.
    float m = 2.0 * sqrt(r.x*r.x + r.y*r.y + (r.z + 1.0)*(r.z + 1.0));
    vec2 uv = vec2(r.x / m + 0.5, r.y / m + 0.5);

    vec4 c = texture(u_reflect, uv) * v_tint;

    float f = fog_factor(v_fogdist);
    c.rgb = mix(pc.fog_color.rgb, c.rgb, f);
    frag = c;
}
