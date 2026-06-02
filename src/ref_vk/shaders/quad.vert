#version 450

// Vertex layout matches vk_vertex2d_t:
//   pos.xy in pixels (0..viddef.width, 0..viddef.height)
//   uv.xy  in 0..1
//   color  RGBA8 normalized
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

// Push constants give us the screen dimensions so we can convert pixels to NDC.
layout(push_constant) uniform PC {
    vec2 viewport_size;     // viddef.width, viddef.height
} pc;

void main() {
    // Convert pixel-space input to NDC: (-1..+1, -1..+1) with Y flipped.
    // Vulkan NDC has Y going DOWN like screen-space pixels, so this is a
    // straight scale-and-bias.
    vec2 ndc = (in_pos / pc.viewport_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv    = in_uv;
    v_color = in_color;
}
