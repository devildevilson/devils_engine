#version 450

// Алгоритм: каждый glyph — один instance и одна собственная world matrix. CPU layout переводит plane bounds
// Crimson в размер квада и располагает его вдоль прямой либо касательной quadratic Bézier. Vertex shader
// ничего не знает о строке: это сохраняет один renderer для любого spatial glyph layout.

layout(location = 0) in vec2 in_position;
layout(location = 1) in mat4 in_glyph_world;
layout(location = 5) in vec4 in_uv_rect;
layout(location = 6) in vec4 in_fill_color;
layout(location = 7) in vec4 in_outline_color;
layout(location = 8) in vec4 in_effect;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_fill_color;
layout(location = 2) out vec4 out_outline_color;
layout(location = 3) out vec3 out_effect;
layout(location = 4) flat out uint out_atlas_index;
layout(location = 5) out vec2 out_detail_uv;
layout(location = 6) flat out uint out_detail_index;
layout(location = 7) flat out float out_detail_mix;

void main() {
  gl_Position = camera_data.view_projection * in_glyph_world * vec4(in_position, 0.0, 1.0);
  out_uv = mix(in_uv_rect.xy, in_uv_rect.zw, in_position);
  out_fill_color = in_fill_color;
  out_outline_color = in_outline_color;
  out_effect = in_effect.xyz;
  const uint packed_textures = floatBitsToUint(in_effect.w);
  out_atlas_index = packed_textures & 0xffu;
  out_detail_uv = in_position;
  out_detail_index = (packed_textures >> 8u) & 0xffu;
  out_detail_mix = float((packed_textures >> 16u) & 0xffu) / 255.0;
}
